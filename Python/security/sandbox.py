"""Filesystem sandbox helpers for the Unreal MCP server."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable


def _normalize(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=False)
    except RuntimeError:
        resolved = path
    if os.name == "nt":
        return Path(str(resolved).replace("\\", "/").lower())
    return resolved


def normalize_path(path: str) -> Path:
    """Return a canonical Path for ``path`` suitable for comparisons."""

    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = (Path.cwd() / candidate).resolve()
    return _normalize(candidate)


def is_within(path: Path, roots: Iterable[str]) -> bool:
    normalized_path = normalize_path(str(path))
    for root in roots:
        root_path = normalize_path(root)
        try:
            normalized_path.relative_to(root_path)
            return True
        except ValueError:
            continue
    return False


def is_path_allowed(path: str, allowed_roots: Iterable[str], forbidden_roots: Iterable[str]) -> bool:
    normalized = normalize_path(path)
    if forbidden_roots and is_within(normalized, forbidden_roots):
        return False
    if not allowed_roots:
        return True
    return is_within(normalized, allowed_roots)


__all__ = ["is_path_allowed", "normalize_path"]
