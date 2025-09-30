"""Lightweight structured logging helpers for the MCP server."""
from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from typing import Any, Dict, Optional

_MAX_FILE_BYTES = 20 * 1024 * 1024
_MAX_GENERATIONS = 3
_LOCK = threading.RLock()
_EVENTS_PATH: Optional[Path] = None
_METRICS_PATH: Optional[Path] = None


def init(directory: Path | str, enable: bool = True) -> None:
    """Initialise the structured log writers."""
    global _EVENTS_PATH, _METRICS_PATH

    if not enable:
        _EVENTS_PATH = None
        _METRICS_PATH = None
        return

    base = Path(directory)
    base.mkdir(parents=True, exist_ok=True)
    _EVENTS_PATH = base / "events.jsonl"
    _METRICS_PATH = base / "metrics.jsonl"


def _rotate(path: Path) -> None:
    if not path.exists():
        return
    if path.stat().st_size < _MAX_FILE_BYTES:
        return

    for index in range(_MAX_GENERATIONS - 1, -1, -1):
        source = path if index == 0 else path.with_suffix(path.suffix + f".{index}")
        if not source.exists():
            continue
        if index == _MAX_GENERATIONS - 1:
            source.unlink(missing_ok=True)
            continue
        dest = path.with_suffix(path.suffix + f".{index + 1}")
        source.rename(dest)


def _write_line(path: Optional[Path], payload: Dict[str, Any]) -> None:
    if path is None:
        return
    with _LOCK:
        _rotate(path)
        with path.open("a", encoding="utf-8") as handle:
            json.dump(payload, handle, separators=(",", ":"))
            handle.write("\n")


def log_event(
    level: str,
    category: str,
    message: str,
    request_id: Optional[str] = None,
    session_id: Optional[str] = None,
    fields: Optional[Dict[str, Any]] = None,
    ts_ms: Optional[float] = None,
) -> None:
    """Append an event entry to the structured log."""
    payload: Dict[str, Any] = {
        "level": (level or "info").lower(),
        "category": category,
        "message": message,
    }
    if request_id:
        payload["requestId"] = request_id
    if session_id:
        payload["sessionId"] = session_id
    if ts_ms is None:
        ts_ms = _now_ms()
    payload["ts"] = ts_ms
    if fields:
        payload["fields"] = fields
    _write_line(_EVENTS_PATH, payload)


def log_metric(name: str, fields: Optional[Dict[str, Any]] = None) -> None:
    """Append a metric entry to the structured log."""
    payload: Dict[str, Any] = {
        "metric": name,
        "ts": _now_ms(),
    }
    if fields:
        payload["fields"] = fields
    _write_line(_METRICS_PATH, payload)


def _now_ms() -> float:
    return time.time() * 1000.0


import time  # placed at end to avoid circular import during module load

