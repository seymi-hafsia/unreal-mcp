"""Utilities for the Unreal MCP framed JSON protocol (Protocol v1)."""

from __future__ import annotations

import json
import socket
import struct
import time
from typing import Any, Dict, Optional

HEADER_SIZE = 4
MAX_FRAME_SIZE = 4 * 1024 * 1024  # 4 MiB safety limit


class ProtocolError(Exception):
    """Raised when a protocol level error occurs."""

    def __init__(self, code: str, message: str, details: Optional[Dict[str, Any]] = None):
        super().__init__(message)
        self.code = code
        self.details = details or {}

    def to_dict(self) -> Dict[str, Any]:
        return {
            "ok": False,
            "error": {
                "code": self.code,
                "message": str(self),
                "details": self.details,
            },
        }


def _wait_for_socket(sock: socket.socket, timeout: Optional[float]) -> None:
    if timeout is not None:
        sock.settimeout(timeout)


def _monotonic_deadline(timeout: Optional[float]) -> Optional[float]:
    if timeout is None:
        return None
    return time.monotonic() + timeout


def _remaining_time(deadline: Optional[float]) -> Optional[float]:
    if deadline is None:
        return None
    remaining = deadline - time.monotonic()
    return max(0.0, remaining)


def read_exact(sock: socket.socket, size: int, timeout: Optional[float] = None) -> bytes:
    """Read exactly ``size`` bytes from ``sock`` respecting ``timeout``."""

    if size <= 0:
        return b""

    deadline = _monotonic_deadline(timeout)
    chunks: list[bytes] = []
    remaining = size

    while remaining > 0:
        chunk_timeout = _remaining_time(deadline)
        try:
            _wait_for_socket(sock, chunk_timeout)
            chunk = sock.recv(remaining)
        except socket.timeout as exc:  # pragma: no cover - depends on OS timing
            raise ProtocolError("READ_TIMEOUT", "Timed out while reading from socket.") from exc
        except OSError as exc:  # pragma: no cover - rare transport errors
            raise ProtocolError("MALFORMED_FRAME", f"Socket read failed: {exc}") from exc

        if not chunk:
            raise ProtocolError("MALFORMED_FRAME", "Socket closed while reading data.")

        chunks.append(chunk)
        remaining -= len(chunk)

    return b"".join(chunks)


def write_all(sock: socket.socket, data: bytes, timeout: Optional[float] = None) -> None:
    """Write all bytes to the socket."""

    if not data:
        return

    deadline = _monotonic_deadline(timeout)
    view = memoryview(data)
    total_sent = 0

    while total_sent < len(data):
        chunk_timeout = _remaining_time(deadline)
        try:
            _wait_for_socket(sock, chunk_timeout)
            sent = sock.send(view[total_sent:])
        except socket.timeout as exc:  # pragma: no cover - depends on OS timing
            raise ProtocolError("WRITE_ERROR", "Timed out while writing to socket.") from exc
        except OSError as exc:
            raise ProtocolError("WRITE_ERROR", f"Socket send failed: {exc}") from exc

        if sent == 0:
            raise ProtocolError("WRITE_ERROR", "Socket closed while writing data.")

        total_sent += sent


def write_frame(sock: socket.socket, payload: Dict[str, Any], timeout: Optional[float] = None) -> None:
    """Encode ``payload`` as JSON and send it as a framed message."""

    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_FRAME_SIZE:
        raise ProtocolError("MALFORMED_FRAME", "Payload exceeds maximum frame size.", {"length": len(body)})

    header = struct.pack("<I", len(body))
    write_all(sock, header, timeout)
    write_all(sock, body, timeout)


def read_frame(sock: socket.socket, timeout: Optional[float] = None) -> Dict[str, Any]:
    """Read a single framed JSON message from ``sock``."""

    header = read_exact(sock, HEADER_SIZE, timeout)
    (length,) = struct.unpack("<I", header)
    if length == 0 or length > MAX_FRAME_SIZE:
        raise ProtocolError("MALFORMED_FRAME", "Invalid frame length.", {"length": length})

    payload = read_exact(sock, length, timeout)
    try:
        return json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ProtocolError("MALFORMED_FRAME", "Invalid JSON payload.") from exc


def make_error(code: str, message: str, details: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Create a standard error response payload."""

    return {
        "ok": False,
        "error": {
            "code": code,
            "message": message,
            "details": details or {},
        },
    }


def current_timestamp_ms() -> int:
    """Return the current Unix timestamp in milliseconds."""

    return int(time.time() * 1000)
