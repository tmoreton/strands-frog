"""WAV validation and Strands setup for the hosted voice endpoint."""

import struct
from collections.abc import Callable

MAX_MESSAGE_CHARS = 500
MAX_REPLY_CHARS = 500
VOICE_SAMPLE_RATE = 16000
VOICE_SAMPLE_BYTES = 2
VOICE_HEADER = struct.Struct("<4sI4s4sIHHIIHH4sI")
MIN_VOICE_BYTES = VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES // 4
MAX_VOICE_BYTES = VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES * 12
MAX_VOICE_BODY_BYTES = VOICE_HEADER.size + MAX_VOICE_BYTES
MAX_SPEECH_BYTES = VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES * 20
DISPLAY_CONTROL_CHARS = dict.fromkeys(code for code in range(32) if code != 10)
OPENROUTER_URL = "https://openrouter.ai/api/v1"
OPENROUTER_MODEL = "deepseek/deepseek-v4.1-flash-1017"
SYSTEM_PROMPT = (
    "You are a friendly assistant on a tiny round display. Reply in one or two short "
    "sentences using plain English text without markdown or emoji."
)


def valid_voice_wav(audio: bytes) -> bool:
    payload_size = len(audio) - VOICE_HEADER.size
    if not MIN_VOICE_BYTES <= payload_size <= MAX_VOICE_BYTES or payload_size % VOICE_SAMPLE_BYTES:
        return False
    return VOICE_HEADER.unpack_from(audio) == (
        b"RIFF", len(audio) - 8, b"WAVE", b"fmt ", 16, 1, 1, VOICE_SAMPLE_RATE,
        VOICE_SAMPLE_RATE * VOICE_SAMPLE_BYTES, VOICE_SAMPLE_BYTES, 16, b"data", payload_size,
    )


def openrouter_answer(api_key: str, model_id: str, *, timeout: float) -> Callable[[str], str]:
    from strands import Agent
    from strands.models.openai import OpenAIModel

    model = OpenAIModel(
        client_args={
            "api_key": api_key,
            "base_url": OPENROUTER_URL,
            "timeout": timeout,
            "max_retries": 0,
        },
        model_id=model_id,
        params={
            "max_tokens": 100,
            "extra_body": {
                "reasoning": {"enabled": False},
                "provider": {"zdr": True},
            },
        },
    )

    def answer(message: str) -> str:
        agent = Agent(
            model=model,
            system_prompt=SYSTEM_PROMPT,
            callback_handler=None,
            retry_strategy=None,
        )
        result = agent(message)
        return "\n".join(block["text"] for block in result.message["content"] if "text" in block)

    return answer
