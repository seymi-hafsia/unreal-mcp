import json
from typing import Any, Dict

import pytest

from protocol import ProtocolError, current_timestamp_ms, read_frame, write_frame


class FakeSocket:
    def __init__(self, initial: bytes | None = None) -> None:
        self._buffer = bytearray(initial or b"")
        self._read_offset = 0
        self._timeout = None

    def settimeout(self, timeout):  # pragma: no cover - setter does not affect tests
        self._timeout = timeout

    def send(self, data: bytes) -> int:
        self._buffer.extend(data)
        return len(data)

    def recv(self, size: int) -> bytes:
        if self._read_offset >= len(self._buffer):
            return b""
        end = min(self._read_offset + size, len(self._buffer))
        chunk = bytes(self._buffer[self._read_offset:end])
        self._read_offset = end
        return chunk

    # Helpers for tests
    def buffer(self) -> bytes:
        return bytes(self._buffer)


def test_write_and_read_frame_roundtrip():
    payload: Dict[str, Any] = {"type": "test", "value": 42}
    writer = FakeSocket()
    write_frame(writer, payload)

    reader = FakeSocket(writer.buffer())
    result = read_frame(reader)
    assert result == payload


def test_read_frame_invalid_length_raises():
    writer = FakeSocket()
    # Write header with invalid huge length
    writer.send((9999999).to_bytes(4, "little"))
    reader = FakeSocket(writer.buffer())

    with pytest.raises(ProtocolError) as exc:
        read_frame(reader)
    assert exc.value.code == "MALFORMED_FRAME"


def test_read_frame_truncated_payload():
    payload = json.dumps({"type": "partial"}).encode("utf-8")
    length = len(payload) + 10
    data = length.to_bytes(4, "little") + payload
    reader = FakeSocket(data)

    with pytest.raises(ProtocolError) as exc:
        read_frame(reader)
    assert exc.value.code == "MALFORMED_FRAME"


def test_write_frame_too_large():
    big_payload = {"data": "x" * (5 * 1024 * 1024)}  # 5 MiB
    writer = FakeSocket()
    with pytest.raises(ProtocolError) as exc:
        write_frame(writer, big_payload)
    assert exc.value.code == "MALFORMED_FRAME"


def test_current_timestamp_ms():
    ts = current_timestamp_ms()
    assert isinstance(ts, int)
    assert ts > 0
