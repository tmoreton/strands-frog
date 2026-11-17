import base64
import io
import json
import struct
import time
import unittest
import wave
from types import SimpleNamespace
from unittest.mock import patch

from handler import VoiceServices, configured_services, handle_request, lambda_handler
from voice_support import MAX_SPEECH_BYTES, MAX_VOICE_BODY_BYTES

TOKEN = "0" * 48


def recording(frames=8000):
    wav = io.BytesIO()
    with wave.open(wav, "wb") as file:
        file.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        file.writeframes(b"\x01\x00" * frames)
    return wav.getvalue()


def voice_event(audio, *, token=TOKEN, content_type="audio/wav"):
    return {
        "version": "2.0",
        "rawPath": "/voice",
        "requestContext": {"http": {"method": "POST"}},
        "headers": {"Authorization": f"Bearer {token}", "Content-Type": content_type},
        "isBase64Encoded": True,
        "body": base64.b64encode(audio).decode("ascii"),
    }


def unpack_reply(response):
    body = base64.b64decode(response["body"], validate=True)
    header_length = struct.unpack_from("<I", body)[0]
    return json.loads(body[4:4 + header_length]), body[4 + header_length:]


class CloudVoiceTest(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.audio = recording()
        self.transcript = "How are you?"
        self.reply = "Très bien."
        self.pcm = b"\x05\x00\xff\xff"

    def transcribe(self, audio):
        self.calls.append("transcribe")
        self.assertEqual(audio, self.audio)
        return self.transcript

    def answer(self, transcript):
        self.calls.append("answer")
        self.assertEqual(transcript, self.transcript)
        return self.reply

    def synthesize(self, reply):
        self.calls.append("synthesize")
        self.assertEqual(reply, self.reply)
        if isinstance(self.pcm, Exception):
            raise self.pcm
        return self.pcm

    def services(self, transcribe_only):
        if transcribe_only:
            return VoiceServices(self.transcribe)
        return VoiceServices(self.transcribe, self.answer, self.synthesize)

    def invoke(self, event=None, *, budget=27):
        return handle_request(event or voice_event(self.audio), TOKEN, self.services, time.monotonic() + budget)

    def test_binary_envelope(self):
        response = self.invoke()
        self.assertEqual(response["statusCode"], 200)
        self.assertTrue(response["isBase64Encoded"])
        self.assertEqual(response["headers"]["content-type"], "application/octet-stream")
        self.assertEqual(response["headers"]["cache-control"], "no-store")
        self.assertEqual(unpack_reply(response), ({"reply": self.reply, "transcript": self.transcript}, self.pcm))
        self.assertEqual(self.calls, ["transcribe", "answer", "synthesize"])

    def test_bad_requests_do_not_call_providers(self):
        self.assertEqual(self.invoke(voice_event(self.audio, token="wrong"))["statusCode"], 401)
        self.assertEqual(self.invoke(voice_event(self.audio, content_type="application/json"))["statusCode"], 415)
        self.assertEqual(self.invoke({**voice_event(self.audio), "body": "!!"})["statusCode"], 400)
        self.assertEqual(self.invoke(voice_event(self.audio[:-1]))["statusCode"], 400)
        self.assertEqual(self.invoke(voice_event(b"x" * (MAX_VOICE_BODY_BYTES + 1)))["statusCode"], 413)
        self.assertEqual(self.calls, [])

    def test_other_routes_do_not_call_providers(self):
        event = {"rawPath": "/voice", "requestContext": {"http": {"method": "GET"}}}
        self.assertEqual(self.invoke(event)["statusCode"], 404)
        self.assertEqual(self.invoke({**voice_event(self.audio), "rawPath": "/chat"})["statusCode"], 404)
        self.assertEqual(self.calls, [])

    def test_empty_transcript_returns_json_error(self):
        self.transcript = " \t\n "
        for mode in ("", "transcribe"):
            with self.subTest(mode=mode):
                event = voice_event(self.audio)
                event["headers"]["X-Strands-Mode"] = mode
                response = self.invoke(event)
                self.assertEqual(response["statusCode"], 422)
                self.assertEqual(json.loads(response["body"]), {"error": "No speech detected. Try again."})
        self.assertEqual(self.calls, ["transcribe", "transcribe"])

    def test_transcribe_only_returns_json_without_initializing_agent(self):
        event = voice_event(self.audio)
        event["headers"]["X-Strands-Mode"] = " Transcribe "
        context = SimpleNamespace(get_remaining_time_in_millis=lambda: 28000)
        configured_services.cache_clear()
        self.addCleanup(configured_services.cache_clear)
        with (
            patch("handler.configured_credentials", return_value=(TOKEN, "test-api-key")),
            patch("handler.openrouter_audio", return_value=b'{"text":" How are you? "}') as audio,
            patch("handler.openrouter_answer", side_effect=RuntimeError) as answer,
        ):
            response = lambda_handler(event, context)
            answer.assert_not_called()
            audio.assert_called_once()
            self.assertEqual(audio.call_args.args[1], "/audio/transcriptions")
        self.assertEqual(response["statusCode"], 200)
        self.assertEqual(response["headers"]["content-type"], "application/json; charset=utf-8")
        self.assertFalse(response.get("isBase64Encoded", False))
        self.assertEqual(json.loads(response["body"]), {"transcript": self.transcript})

    def test_speech_failures_preserve_text(self):
        for pcm in (ValueError("private detail"), b"x", b"\x00" * (MAX_SPEECH_BYTES + 2)):
            self.pcm = pcm
            response = self.invoke()
            self.assertEqual(response["statusCode"], 200)
            self.assertEqual(unpack_reply(response), ({"reply": self.reply, "transcript": self.transcript}, b""))

    def test_speech_skipped_near_deadline(self):
        response = self.invoke(budget=1)
        self.assertEqual(response["statusCode"], 200)
        self.assertEqual(unpack_reply(response)[1], b"")
        self.assertEqual(self.calls, ["transcribe", "answer"])

    def test_speech_skipped_when_disabled(self):
        event = voice_event(self.audio)
        event["headers"]["X-Strands-TTS"] = " Off "
        response = self.invoke(event)
        self.assertEqual(response["statusCode"], 200)
        self.assertEqual(unpack_reply(response), ({"reply": self.reply, "transcript": self.transcript}, b""))
        self.assertEqual(self.calls, ["transcribe", "answer"])


if __name__ == "__main__":
    unittest.main()
