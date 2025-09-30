"""Placeholder implementation for MetaSound offline rendering."""

from __future__ import annotations

from typing import Any, Dict


class MetaSoundRenderError(RuntimeError):
    """Raised when offline rendering is not available."""


def run(payload: Dict[str, Any]) -> Dict[str, Any]:
    """Entry point for metasound.render_offline (not implemented)."""

    return {
        "ok": False,
        "error": {
            "code": "NOT_IMPLEMENTED",
            "message": "metasound.render_offline is not available in this build.",
        },
    }


__all__ = ["run", "MetaSoundRenderError"]

