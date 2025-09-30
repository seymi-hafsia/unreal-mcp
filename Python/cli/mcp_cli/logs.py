"""Logging utilities for the MCP CLI."""

from __future__ import annotations

import json
import logging
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Optional

from rich.console import Console
from rich.logging import RichHandler

__all__ = ["configure_logging", "get_logger"]

_DEFAULT_JSONL = Path.home() / ".mcp-cli" / "events.jsonl"


class _JsonlHandler(logging.Handler):
    """Write log records as JSON lines to a file."""

    def __init__(self, path: Path) -> None:
        super().__init__()
        self._path = path
        self._path.parent.mkdir(parents=True, exist_ok=True)

    def emit(self, record: logging.LogRecord) -> None:  # pragma: no cover - filesystem IO
        payload: Dict[str, Any] = {
            "ts": datetime.now(timezone.utc).isoformat(),
            "level": record.levelname.lower(),
            "msg": record.getMessage(),
        }
        for key in ("step", "tool", "request_id", "ok", "duration_ms"):
            if hasattr(record, key):
                payload[key] = getattr(record, key)
        if record.exc_info:
            payload["exc_info"] = logging.Formatter().formatException(record.exc_info)
        with self._path.open("a", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False)
            handle.write("\n")


def _level_from_str(level: str) -> int:
    mapping = {
        "critical": logging.CRITICAL,
        "error": logging.ERROR,
        "warning": logging.WARNING,
        "info": logging.INFO,
        "debug": logging.DEBUG,
        "trace": logging.NOTSET,
    }
    normalized = level.lower()
    return mapping.get(normalized, logging.INFO)


def configure_logging(level: str = "info", jsonl_path: Optional[Path] = None) -> logging.Logger:
    """Configure structured logging for the CLI."""

    logger = logging.getLogger("mcp_cli")
    logger.setLevel(_level_from_str(level))
    logger.propagate = False
    logger.handlers.clear()

    console = Console(stderr=True)
    console_handler = RichHandler(
        console=console,
        show_time=False,
        show_path=False,
        markup=True,
        enable_link_path=False,
    )
    console_handler.setLevel(logger.level)
    formatter = logging.Formatter("%(message)s")
    console_handler.setFormatter(formatter)
    logger.addHandler(console_handler)

    jsonl_target = jsonl_path or _DEFAULT_JSONL
    logger.addHandler(_JsonlHandler(jsonl_target))

    return logger


def get_logger(name: str) -> logging.Logger:
    """Return a child logger of the CLI root logger."""

    return logging.getLogger("mcp_cli").getChild(name)
