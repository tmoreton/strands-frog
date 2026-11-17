"""API Gateway v2 handler for the Waveshare voice gadget."""

import base64
import binascii
import json
import logging
import os
import re
import secrets
import struct
import time
from collections.abc import Callable
from dataclasses import dataclass
from functools import cache
from urllib.request import Request, urlopen

from voice_support import (
    DISPLAY_CONTROL_CHARS,
    MAX_MESSAGE_CHARS,
    MAX_REPLY_CHARS,
    MAX_SPEECH_BYTES,
    MAX_VOICE_BODY_BYTES,
    OPENROUTER_MODEL,
    OPENROUTER_URL,
    openrouter_answer,
    valid_voice_wav,
)

TRANSCRIPTION_MODEL = "openai/whisper-large-v3-turbo"
SPEECH_MODEL = "elevenlabs/eleven-flash-v2.5"
STT_TIMEOUT = 8
CHAT_TIMEOUT = 10
TTS_TIMEOUT = 6
MAX_ENCODED_VOICE_BODY = 4 * ((MAX_VOICE_BODY_BYTES + 2) // 3)
LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class VoiceServices:
    transcribe: Callable[[bytes], str]
    answer: Callable[[str], str] | None = None
    synthesize: Callable[[str], bytes] | None = None


def json_response(status: int, payload: dict) -> dict:
    return {
        "statusCode": status,
        "headers": {"content-type": "application/json; charset=utf-8", "cache-control": "no-store"},
        "body": json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
    }


def voice_response(reply: str, transcript: str, pcm: bytes) -> dict:
    header = json.dumps({"reply": reply, "transcript": transcript}, ensure_ascii=False, separators=(",", ":")).encode()
    body = struct.pack("<I", len(header)) + header + pcm
    return {
        "statusCode": 200,
        "headers": {"content-type": "application/octet-stream", "cache-control": "no-store"},
        "isBase64Encoded": True,
        "body": base64.b64encode(body).decode("ascii"),
    }


def handle_request(
    event: dict,
    token: str,
    get_services: Callable[[bool], VoiceServices],
    deadline: float,
) -> dict:
    method = event.get("requestContext", {}).get("http", {}).get("method")
    path = event.get("rawPath")
    if method != "POST" or path != "/voice":
        return json_response(404, {"error": "Route not found"})
    if not re.fullmatch(r"[0-9a-fA-F]{32,64}", token):
        return json_response(503, {"error": "Voice is unavailable"})

    headers = {key.lower(): value for key, value in (event.get("headers") or {}).items()}
    authorization = headers.get("authorization", "").encode("utf-8")
    if not secrets.compare_digest(authorization, f"Bearer {token}".encode("ascii")):
        return json_response(401, {"error": "Device token rejected"})
    if headers.get("content-type", "").split(";", 1)[0].strip().lower() != "audio/wav":
        return json_response(415, {"error": "Expected audio/wav"})

    body = event.get("body")
    if not isinstance(body, str) or not event.get("isBase64Encoded"):
        return json_response(400, {"error": "Expected WAV bytes"})
    if len(body) > MAX_ENCODED_VOICE_BODY:
        return json_response(413, {"error": "Invalid body size"})
    try:
        audio = base64.b64decode(body, validate=True)
    except (binascii.Error, ValueError):
        return json_response(400, {"error": "Invalid WAV payload"})
    if len(audio) > MAX_VOICE_BODY_BYTES:
        return json_response(413, {"error": "Invalid body size"})
    if not valid_voice_wav(audio):
        return json_response(400, {"error": "Expected 0.25 to 12 seconds of 16 kHz mono PCM16 WAV"})

    transcribe_only = headers.get("x-strands-mode", "").strip().lower() == "transcribe"
    try:
        services = get_services(transcribe_only)
    except Exception as error:
        LOGGER.warning("Voice setup failed (%s)", type(error).__name__)
        return json_response(503, {"error": "Voice is unavailable"})

    try:
        transcript = services.transcribe(audio).translate(DISPLAY_CONTROL_CHARS).strip()[:MAX_MESSAGE_CHARS]
    except Exception as error:
        LOGGER.warning("Transcription failed (%s)", type(error).__name__)
        return json_response(502, {"error": "Transcription unavailable. Try again."})
    if not transcript:
        return json_response(422, {"error": "No speech detected. Try again."})
    if transcribe_only:
        return json_response(200, {"transcript": transcript})
    if services.answer is None or services.synthesize is None:
        return json_response(503, {"error": "Voice is unavailable"})

    try:
        reply = services.answer(transcript).translate(DISPLAY_CONTROL_CHARS).strip()[:MAX_REPLY_CHARS]
    except Exception as error:
        LOGGER.warning("Chat failed (%s)", type(error).__name__)
        return json_response(502, {"error": "Agent unavailable. Try again."})
    if not reply:
        return json_response(502, {"error": "Agent returned no text"})

    pcm = b""
    tts_enabled = headers.get("x-strands-tts", "").strip().lower() != "off"
    if tts_enabled and deadline - time.monotonic() > TTS_TIMEOUT + 1:
        try:
            speech = services.synthesize(reply)
            if isinstance(speech, bytes) and 0 < len(speech) <= MAX_SPEECH_BYTES and len(speech) % 2 == 0:
                pcm = speech
        except Exception as error:
            LOGGER.warning("Speech synthesis failed (%s)", type(error).__name__)
    return voice_response(reply, transcript, pcm)


def openrouter_audio(api_key: str, path: str, payload: dict, timeout: int, content_type: str, max_bytes: int) -> bytes:
    request = Request(
        OPENROUTER_URL + path,
        data=json.dumps(payload, separators=(",", ":")).encode(),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request, timeout=timeout) as response:
        if response.headers.get_content_type() != content_type:
            raise ValueError("Unexpected audio service response")
        result = response.read(max_bytes + 1)
        if len(result) > max_bytes:
            raise ValueError("Audio service response is too large")
        return result


@cache
def configured_credentials() -> tuple[str, str]:
    secret_arn = os.getenv("VOICE_SECRET_ARN")
    settings = os.environ
    if secret_arn:
        import boto3
        from botocore.config import Config

        client = boto3.client("secretsmanager", config=Config(
            connect_timeout=2, read_timeout=4, retries={"total_max_attempts": 1, "mode": "standard"},
        ))
        settings = json.loads(client.get_secret_value(SecretId=secret_arn)["SecretString"])
    token = settings["DEMO_TOKEN"]
    api_key = settings["OPENROUTER_API_KEY"]
    if not isinstance(token, str) or not isinstance(api_key, str) or not api_key.strip():
        raise ValueError("Voice credentials unavailable")
    return token, api_key.strip()


@cache
def configured_services(transcribe_only: bool) -> VoiceServices:
    _, api_key = configured_credentials()

    def transcribe(audio: bytes) -> str:
        payload = {
            "model": TRANSCRIPTION_MODEL,
            "input_audio": {"data": base64.b64encode(audio).decode("ascii"), "format": "wav"},
            "language": "en",
            "provider": {"zdr": True},
        }
        result = openrouter_audio(api_key, "/audio/transcriptions", payload, STT_TIMEOUT, "application/json", 65536)
        return json.loads(result)["text"]

    if transcribe_only:
        return VoiceServices(transcribe)

    def synthesize(reply: str) -> bytes:
        payload = {
            "model": SPEECH_MODEL,
            "input": reply,
            "voice": "sarah",
            "response_format": "pcm",
            "provider": {"zdr": True, "options": {"elevenlabs": {"output_format": "pcm_16000"}}},
        }
        return openrouter_audio(api_key, "/audio/speech", payload, TTS_TIMEOUT, "audio/pcm", MAX_SPEECH_BYTES)

    logging.getLogger("strands").setLevel(logging.CRITICAL)
    answer = openrouter_answer(api_key, os.getenv("OPENROUTER_MODEL_ID") or OPENROUTER_MODEL, timeout=CHAT_TIMEOUT)
    return VoiceServices(transcribe, answer, synthesize)


def lambda_handler(event: dict, context) -> dict:
    budget = min(context.get_remaining_time_in_millis() / 1000, 27)
    deadline = time.monotonic() + budget
    token = ""
    if event.get("rawPath") == "/voice" and event.get("requestContext", {}).get("http", {}).get("method") == "POST":
        try:
            token, _ = configured_credentials()
        except Exception as error:
            LOGGER.warning("Voice credentials unavailable (%s)", type(error).__name__)
            return json_response(503, {"error": "Voice is unavailable"})
    return handle_request(event, token, configured_services, deadline)
