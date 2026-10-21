"""Loopback-only EZ HTTP to Paraformer WebSocket adapter (Python standard library)."""
from __future__ import annotations

import argparse
import base64
import binascii
import hmac
import io
import json
import os
from pathlib import Path
import socket
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from paraformer import _run_paraformer_realtime

MAX_PCM_BYTES = 60 * 16000 * 2
MAX_REQUEST_BYTES = 3 * 1024 * 1024
ASR_PATH = "/api/v1/services/aigc/multimodal-generation/generation"


def decode_audio(payload: dict, model: str) -> bytes:
    if payload.get("model") != model:
        raise ValueError("unexpected ASR model")
    data = payload["input"]["messages"][0]["content"][0]["input_audio"]["data"]
    prefix = "data:audio/wav;base64,"
    if not isinstance(data, str) or not data.startswith(prefix):
        raise ValueError("expected inline WAV")
    wav_bytes = base64.b64decode(data[len(prefix):], validate=True)
    with wave.open(io.BytesIO(wav_bytes)) as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, 16000, "NONE"):
            raise ValueError("expected 16kHz mono PCM16")
        if not 0 < wav.getnframes() <= MAX_PCM_BYTES // 2:
            raise ValueError("audio must be between 0 and 60 seconds")
        pcm = wav.readframes(wav.getnframes())
        if len(pcm) != wav.getnframes() * 2:
            raise ValueError("truncated WAV")
    return pcm


class SpeechServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 2

    def __init__(self, address, config):
        self.config = config
        self.slots = threading.BoundedSemaphore(2)
        super().__init__(address, SpeechHandler)

    def process_request(self, request, client_address):
        # Bound worker creation as well as provider concurrency.
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class SpeechHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def log_message(self, *_):
        pass  # Never log audio, credentials, or provider response bodies.

    def reply(self, status, body):
        encoded = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.reply(200 if self.path == "/health" else 404, {"service": "esp-box3-ez-asr", "version": 1})

    def do_POST(self):
        config = self.server.config
        if self.path != ASR_PATH:
            return self.reply(404, {"code": "NotFound"})
        expected = "Bearer " + config["XOB_BAILIAN_API_KEY"]
        if not hmac.compare_digest(self.headers.get("Authorization", "").encode(), expected.encode()):
            return self.reply(401, {"code": "Unauthorized"})
        try:
            if self.headers.get("Transfer-Encoding") or len(self.headers.get_all("Content-Length", [])) != 1:
                raise ValueError("expected one Content-Length")
            size = int(self.headers["Content-Length"])
            if not 0 < size <= MAX_REQUEST_BYTES:
                raise ValueError("request too large")
            raw = self.rfile.read(size)
            if len(raw) != size:
                raise ValueError("truncated request")
            payload = json.loads(raw)
            pcm = decode_audio(payload, config["XOB_BAILIAN_PARA_ASR_MODEL"])
        except (ValueError, TypeError, KeyError, IndexError, AttributeError, wave.Error, binascii.Error):
            return self.reply(400, {"code": "InvalidAudio"})
        try:
            text, _ = _run_paraformer_realtime(
                api_key=config["XOB_BAILIAN_API_KEY"],
                base_url=config["XOB_BAILIAN_ASR_WS_URL"],
                model=config["XOB_BAILIAN_PARA_ASR_MODEL"],
                audio=pcm, audio_format="pcm", sample_rate=16000, timeout=25,
            )
        except (OSError, ValueError, socket.timeout):
            return self.reply(502, {"code": "AsrProviderFailed"})
        self.reply(200, {"output": {"text": text}})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--port", type=int, default=18743)
    args = parser.parse_args()
    if args.config.stat().st_mode & 0o077:
        parser.error("private config permissions must be 0600")
    config = json.loads(args.config.read_text())
    for key in ("XOB_BAILIAN_API_KEY", "XOB_BAILIAN_PARA_ASR_MODEL", "XOB_BAILIAN_ASR_WS_URL"):
        if not isinstance(config.get(key), str) or not config[key].strip():
            parser.error("missing speech configuration")
    if config["XOB_BAILIAN_ASR_WS_URL"] != "https://dashscope.aliyuncs.com/api-ws/v1/inference":
        parser.error("expected the verified official ASR endpoint")
    os.environ["XOB_BAILIAN_ASR_WS_URL"] = config["XOB_BAILIAN_ASR_WS_URL"]
    SpeechServer(("127.0.0.1", args.port), config).serve_forever()


if __name__ == "__main__":
    main()
