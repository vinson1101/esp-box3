"""Paraformer transport reused from Vinson's Bridge, commit da7d7c4.

Only the PCM/WAV WebSocket path is retained; no old backend dependency.
"""
from __future__ import annotations
import base64
import hashlib
import json
import os
import socket
import ssl
from typing import Any
from urllib.parse import urlsplit
from uuid import uuid4

def _run_paraformer_realtime(
    api_key: str,
    base_url: str,
    model: str,
    audio: bytes,
    audio_format: str,
    sample_rate: int,
    timeout: int,
) -> tuple[str, int | None]:
    host, path = _bailian_ws_host_path(base_url)
    task_id = str(uuid4())
    sock = _ws_connect(host, path, api_key, timeout)
    try:
        run_task = {
            "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio",
                "task": "asr",
                "function": "recognition",
                "model": model,
                "parameters": {
                    "format": audio_format,
                    "sample_rate": sample_rate,
                    "disfluency_removal_enabled": False,
                    "punctuation_prediction_enabled": True,
                    "semantic_punctuation_enabled": False,
                    "max_sentence_silence": _int_env("XOB_BAILIAN_PARA_MAX_SENTENCE_SILENCE_MS", 800),
                },
                "input": {},
            },
        }
        if os.environ.get("XOB_BAILIAN_PARA_LANGUAGE_HINTS", "zh,en").strip():
            run_task["payload"]["parameters"]["language_hints"] = [
                item.strip()
                for item in os.environ.get("XOB_BAILIAN_PARA_LANGUAGE_HINTS", "zh,en").split(",")
                if item.strip()
            ]
        _ws_send(sock, 1, json.dumps(run_task).encode())
        _wait_ws_event(sock, "task-started")

        chunk_bytes = _int_env("XOB_BAILIAN_PARA_AUDIO_CHUNK_BYTES", 3200)
        for offset in range(0, len(audio), chunk_bytes):
            _ws_send(sock, 2, audio[offset : offset + chunk_bytes])

        _ws_send(
            sock,
            1,
            json.dumps(
                {
                    "header": {"action": "finish-task", "task_id": task_id, "streaming": "duplex"},
                    "payload": {"input": {}},
                }
            ).encode(),
        )
        return _read_paraformer_result(sock)
    finally:
        try:
            sock.close()
        except OSError:
            pass

def _bailian_ws_host_path(base_url: str) -> tuple[str, str]:
    explicit = os.environ.get("XOB_BAILIAN_ASR_WS_URL", "").strip()
    parsed = urlsplit(explicit or base_url)
    host = parsed.netloc or parsed.path
    if not host:
        raise ValueError("Bailian WebSocket host is empty")
    return host, "/api-ws/v1/inference"

def _ws_connect(host: str, path: str, api_key: str, timeout: int) -> ssl.SSLSocket:
    if any(c in host + path + api_key for c in "\r\n"):
        raise ValueError("invalid WebSocket connection parameters")
    raw = socket.create_connection((host, 443), timeout=timeout)
    sock = None
    try:
        sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
        sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Authorization: Bearer {api_key}\r\n"
        "user-agent: esp-box3-ez-speech\r\n"
        "\r\n"
        ).encode()
        sock.sendall(request)
        response = _read_until(sock, b"\r\n\r\n")
        lines = response.decode("ascii").split("\r\n")
        headers = {}
        for line in lines[1:]:
            if not line:
                continue
            name, value = line.split(":", 1)
            name = name.lower()
            if name in headers:
                raise ValueError("duplicate WebSocket response header")
            headers[name] = value.strip()
        expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        if (lines[0].split()[1:2] != ["101"] or headers.get("upgrade", "").lower() != "websocket" or
                "upgrade" not in {v.strip().lower() for v in headers.get("connection", "").split(",")} or
                headers.get("sec-websocket-accept") != expected or headers.get("sec-websocket-extensions")):
            raise ValueError("invalid WebSocket handshake")
        return sock
    except BaseException:
        (sock or raw).close()
        raise

def _wait_ws_event(sock: ssl.SSLSocket, expected: str) -> None:
    while True:
        opcode, payload = _ws_read(sock)
        if opcode != 1:
            continue
        message = _json_loads(payload)
        header = message.get("header") if isinstance(message.get("header"), dict) else {}
        event = header.get("event")
        if event == expected:
            return
        if event == "task-failed":
            raise ValueError(header.get("error_message") or header.get("error_code") or "Paraformer task failed")

def _read_paraformer_result(sock: ssl.SSLSocket) -> tuple[str, int | None]:
    final_texts: list[str] = []
    latest_text = ""
    duration: int | None = None
    while True:
        opcode, payload = _ws_read(sock)
        if opcode != 1:
            continue
        message = _json_loads(payload)
        header = message.get("header") if isinstance(message.get("header"), dict) else {}
        event = header.get("event")
        if event == "task-failed":
            raise ValueError(header.get("error_message") or header.get("error_code") or "Paraformer task failed")
        payload_obj = message.get("payload") if isinstance(message.get("payload"), dict) else {}
        output = payload_obj.get("output") if isinstance(payload_obj.get("output"), dict) else {}
        sentence = output.get("sentence") if isinstance(output.get("sentence"), dict) else {}
        text = sentence.get("text")
        if isinstance(text, str) and text.strip():
            latest_text = text.strip()
            if sentence.get("sentence_end") is True:
                final_texts.append(latest_text)
        usage = payload_obj.get("usage") if isinstance(payload_obj.get("usage"), dict) else {}
        if isinstance(usage.get("duration"), int):
            duration = usage["duration"]
        if event == "task-finished":
            break
    return ("".join(final_texts).strip() or latest_text, duration)

def _json_loads(payload: bytes) -> dict[str, Any]:
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid WebSocket JSON from Bailian: {_truncate(str(exc), 160)}") from exc
    if not isinstance(decoded, dict):
        raise ValueError("invalid WebSocket JSON from Bailian")
    return decoded

def _read_until(sock: socket.socket, marker: bytes) -> bytes:
    data = b""
    while marker not in data:
        if len(data) >= 16384:
            raise ValueError("WebSocket headers too large")
        chunk = sock.recv(1)
        if not chunk:
            raise OSError("truncated WebSocket headers")
        data += chunk
    return data

def _ws_send(sock: socket.socket, opcode: int, payload: bytes) -> None:
    mask = os.urandom(4)
    length = len(payload)
    if length < 126:
        header = bytes([0x80 | opcode, 0x80 | length])
    elif length < 65536:
        header = bytes([0x80 | opcode, 0x80 | 126]) + length.to_bytes(2, "big")
    else:
        header = bytes([0x80 | opcode, 0x80 | 127]) + length.to_bytes(8, "big")
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(header + mask + masked)

def _ws_read(sock: socket.socket) -> tuple[int, bytes]:
    header = _recv_exact(sock, 2)
    first, second = header
    opcode = first & 0x0F
    # shortcut: refuse fragmentation; add bounded reassembly if a provider needs it.
    if not first & 0x80 or first & 0x70 or second & 0x80 or opcode not in {1, 2, 8, 9, 10}:
        raise ValueError("unsupported WebSocket frame")
    length = second & 0x7F
    if length == 126:
        length = int.from_bytes(_recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(_recv_exact(sock, 8), "big")
    if length > 65536 or (opcode >= 8 and length > 125):
        raise ValueError("WebSocket frame too large")
    payload = _recv_exact(sock, length)
    if opcode == 8:
        raise OSError("provider closed WebSocket")
    if opcode == 9:
        _ws_send(sock, 10, payload)
    return opcode, payload

def _recv_exact(sock: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise OSError("truncated websocket frame")
        data += chunk
    return data

def _int_env(name: str, default: int) -> int:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError:
        return default
    return value if value > 0 else default

def _truncate(text: str, limit: int) -> str:
    return text if len(text) <= limit else text[: limit - 3] + "..."
