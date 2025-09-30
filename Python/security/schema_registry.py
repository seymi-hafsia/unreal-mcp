"""JSON Schema registry for sensitive MCP tools."""

from __future__ import annotations

from typing import Any, Dict, Optional

try:
    import jsonschema
except ImportError:  # pragma: no cover - jsonschema is an optional dependency
    jsonschema = None  # type: ignore


SCHEMAS: Dict[str, Dict[str, Any]] = {
    "asset.batch_import": {
        "type": "object",
        "properties": {
            "files": {
                "type": "array",
                "items": {"type": "object"},
                "maxItems": 10000,
            },
            "confirm": {"type": "boolean"},
        },
        "required": ["files"],
        "additionalProperties": True,
    },
    "sequence.create": {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "path": {"type": "string"},
        },
        "required": ["name", "path"],
        "additionalProperties": False,
    },
}


def get_schema(tool: str) -> Optional[Dict[str, Any]]:
    return SCHEMAS.get(tool)


def validate(tool: str, params: Dict[str, Any]) -> Optional[str]:
    schema = get_schema(tool)
    if not schema or not jsonschema:
        return None
    try:
        jsonschema.validate(instance=params, schema=schema)  # type: ignore[arg-type]
    except Exception as exc:  # pragma: no cover - error path
        return str(exc)
    return None


__all__ = ["get_schema", "validate"]
