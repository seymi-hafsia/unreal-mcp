"""Synchronous TCP client for the Unreal MCP Python server."""

from __future__ import annotations

import json
import socket
import struct
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Optional

from tenacity import Retrying, retry_if_exception_type, stop_after_attempt, wait_exponential

from .logs import get_logger

__all__ = ["MCPClient", "ProtocolError"]


logger = get_logger("client")


class ProtocolError(RuntimeError):
    """Raised when a transport level error occurs."""

    def __init__(self, code: str, message: str, details: Optional[Dict[str, Any]] = None) -> None:
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


HEADER_SIZE = 4
MAX_FRAME_SIZE = 4 * 1024 * 1024


@dataclass
class _Endpoint:
    host: str
    port: int


def _parse_endpoint(server: str) -> _Endpoint:
    if "//" in server:
        server = server.split("//", 1)[1]
    if ":" not in server:
        host, port = server, 8765
    else:
        host, port_str = server.rsplit(":", 1)
        port = int(port_str)
    return _Endpoint(host or "127.0.0.1", port)


def _monotonic_deadline(timeout: Optional[float]) -> Optional[float]:
    import time

    if timeout is None:
        return None
    return time.monotonic() + timeout


def _remaining_time(deadline: Optional[float]) -> Optional[float]:
    import time

    if deadline is None:
        return None
    remaining = deadline - time.monotonic()
    return max(0.0, remaining)


def _read_exact(sock: socket.socket, size: int, timeout: Optional[float] = None) -> bytes:
    if size <= 0:
        return b""
    deadline = _monotonic_deadline(timeout)
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk_timeout = _remaining_time(deadline)
        try:
            sock.settimeout(chunk_timeout)
            chunk = sock.recv(remaining)
        except socket.timeout as exc:  # pragma: no cover - depends on OS
            raise ProtocolError("READ_TIMEOUT", "Timed out while reading from MCP server.") from exc
        except OSError as exc:  # pragma: no cover - platform specific
            raise ProtocolError("TRANSPORT_ERROR", f"Socket read failed: {exc}") from exc
        if not chunk:
            raise ProtocolError("CONNECTION_CLOSED", "Socket closed while reading frame.")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _write_all(sock: socket.socket, payload: bytes, timeout: Optional[float] = None) -> None:
    if not payload:
        return
    deadline = _monotonic_deadline(timeout)
    total_sent = 0
    while total_sent < len(payload):
        chunk_timeout = _remaining_time(deadline)
        try:
            sock.settimeout(chunk_timeout)
            sent = sock.send(payload[total_sent:])
        except socket.timeout as exc:  # pragma: no cover - depends on OS
            raise ProtocolError("WRITE_TIMEOUT", "Timed out while sending frame to MCP server.") from exc
        except OSError as exc:  # pragma: no cover
            raise ProtocolError("TRANSPORT_ERROR", f"Socket write failed: {exc}") from exc
        if sent == 0:
            raise ProtocolError("CONNECTION_CLOSED", "Socket closed while sending frame.")
        total_sent += sent


def _write_frame(sock: socket.socket, payload: Dict[str, Any], timeout: Optional[float] = None) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_FRAME_SIZE:
        raise ProtocolError("FRAME_TOO_LARGE", "Frame exceeds maximum size.", {"length": len(body)})
    header = struct.pack("<I", len(body))
    _write_all(sock, header, timeout)
    _write_all(sock, body, timeout)


def _read_frame(sock: socket.socket, timeout: Optional[float] = None) -> Dict[str, Any]:
    header = _read_exact(sock, HEADER_SIZE, timeout)
    (length,) = struct.unpack("<I", header)
    if length <= 0 or length > MAX_FRAME_SIZE:
        raise ProtocolError("MALFORMED_FRAME", "Invalid frame length.", {"length": length})
    payload = _read_exact(sock, length, timeout)
    try:
        return json.loads(payload.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ProtocolError("INVALID_JSON", "Received invalid JSON payload.") from exc


class MCPClient:
    """Client capable of sending tool invocations to the MCP server."""

    def __init__(
        self,
        server: str,
        *,
        connect_timeout: float = 5.0,
        read_timeout: float = 120.0,
    ) -> None:
        self._endpoint = _parse_endpoint(server)
        self._connect_timeout = connect_timeout
        self._read_timeout = read_timeout
        self._sock: Optional[socket.socket] = None
        self._session_id = str(uuid.uuid4())
        self._handshake_ok = False

    @property
    def connected(self) -> bool:
        return self._handshake_ok and self._sock is not None

    def connect(self) -> Dict[str, Any]:
        if self._sock:
            self.close()
        sock = socket.create_connection((self._endpoint.host, self._endpoint.port), timeout=self._connect_timeout)
        sock.settimeout(None)
        self._sock = sock
        handshake = {
            "type": "handshake",
            "client": "mcp-cli",
            "sessionId": self._session_id,
            "protocolVersion": 1,
        }
        _write_frame(sock, handshake, timeout=self._connect_timeout)
        ack = _read_frame(sock, timeout=self._connect_timeout)
        if ack.get("type") != "handshake/ack" or not ack.get("ok", False):
            raise ProtocolError("HANDSHAKE_FAILED", "MCP server rejected handshake.", {"response": ack})
        self._handshake_ok = True
        logger.debug("Handshake accepted: %s", ack)
        return ack

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:  # pragma: no cover - best effort
                pass
        self._sock = None
        self._handshake_ok = False

    def call_tool(
        self,
        tool: str,
        params: Optional[Dict[str, Any]] = None,
        *,
        meta: Optional[Dict[str, Any]] = None,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        if not self._handshake_ok or not self._sock:
            self.connect()
        request_id = str(uuid.uuid4())
        payload = {
            "type": "tool/call",
            "tool": tool,
            "params": params or {},
            "requestId": request_id,
            "idempotencyKey": request_id,
            "meta": meta or {},
        }
        attempt_timeout = timeout or self._read_timeout
        _write_frame(self._sock, payload, timeout=self._connect_timeout)
        response = _read_frame(self._sock, timeout=attempt_timeout)
        return response

    def call_with_retry(
        self,
        tool: str,
        params: Optional[Dict[str, Any]] = None,
        *,
        meta: Optional[Dict[str, Any]] = None,
        attempts: int = 1,
        timeout: Optional[float] = None,
    ) -> Dict[str, Any]:
        def _invoke() -> Dict[str, Any]:
            try:
                return self.call_tool(tool, params, meta=meta, timeout=timeout)
            except (ProtocolError, OSError):
                self.close()
                raise

        if attempts <= 1:
            return _invoke()

        retry = Retrying(
            retry=retry_if_exception_type((ProtocolError, OSError)),
            stop=stop_after_attempt(attempts),
            wait=wait_exponential(multiplier=0.5, min=0.5, max=5.0),
            reraise=True,
        )
        for attempt in retry:
            with attempt:
                return _invoke()
        raise AssertionError("tenacity did not return a result")


__all__ = ["MCPClient", "ProtocolError"]
