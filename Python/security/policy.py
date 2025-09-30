"""Policy enforcement utilities for the Unreal MCP server."""

from __future__ import annotations

import fnmatch
import os
from dataclasses import dataclass, field
from pathlib import Path
from threading import RLock
from typing import Dict, Iterable, List, Optional

import yaml


@dataclass
class RoleRules:
    """Allow and deny patterns for a specific role."""

    allow: List[str] = field(default_factory=list)
    deny: List[str] = field(default_factory=list)

    def evaluate(self, tool: str) -> bool:
        """Return ``True`` when ``tool`` is permitted for the role."""

        normalized_tool = tool or ""
        explicit_allow = False

        patterns = self.allow + [f"!{pattern}" for pattern in self.deny]
        if not patterns:
            return False

        for pattern in patterns:
            pattern = pattern.strip()
            if not pattern:
                continue
            is_deny = pattern.startswith("!")
            if is_deny:
                pattern = pattern[1:].strip()
            if not pattern:
                continue
            if fnmatch.fnmatchcase(normalized_tool, pattern):
                if is_deny:
                    return False
                explicit_allow = True
        return explicit_allow


@dataclass
class PolicyLimits:
    rate_per_minute_global: int = 120
    rate_per_minute_per_tool: int = 30
    request_size_kb: int = 512
    array_items_max: int = 10_000


@dataclass
class PathRules:
    allowed: List[str] = field(default_factory=list)
    forbidden: List[str] = field(default_factory=list)


@dataclass
class AuditRules:
    require_signature: bool = True
    hmac_secret_env: str = "MCP_AUDIT_SECRET"


@dataclass
class Policy:
    roles: Dict[str, RoleRules] = field(default_factory=dict)
    limits: PolicyLimits = field(default_factory=PolicyLimits)
    paths: PathRules = field(default_factory=PathRules)
    audit: AuditRules = field(default_factory=AuditRules)

    def role_rules(self, role: str) -> RoleRules:
        return self.roles.get(role, RoleRules())

    def is_tool_allowed(self, role: str, tool: str) -> bool:
        rules = self.role_rules(role)
        return rules.evaluate(tool)


class PolicyLoader:
    """Load and cache policy documents from disk."""

    def __init__(self, policy_path: Optional[Path] = None) -> None:
        self.policy_path = policy_path or self._resolve_from_env()
        self._policy: Optional[Policy] = None
        self._lock = RLock()

    def _resolve_from_env(self) -> Optional[Path]:
        env_path = os.getenv("MCP_POLICY_PATH")
        if env_path:
            return Path(env_path).expanduser().resolve()
        return None

    def load(self, force: bool = False) -> Policy:
        with self._lock:
            if self._policy is not None and not force:
                return self._policy
            data = self._read_policy()
            self._policy = self._parse_policy(data)
            return self._policy

    def _read_policy(self) -> Dict[str, object]:
        if self.policy_path and self.policy_path.exists():
            with self.policy_path.open("r", encoding="utf-8") as handle:
                return yaml.safe_load(handle) or {}
        return {}

    def _parse_policy(self, data: Dict[str, object]) -> Policy:
        roles_data = data.get("roles", {}) if isinstance(data, dict) else {}
        roles: Dict[str, RoleRules] = {}
        for name, payload in roles_data.items():
            if isinstance(payload, dict):
                allow = [str(p) for p in payload.get("allow", []) if isinstance(p, str)]
                deny = [str(p) for p in payload.get("deny", []) if isinstance(p, str)]
                roles[name] = RoleRules(allow=allow, deny=deny)

        limits_data = data.get("limits", {}) if isinstance(data, dict) else {}
        limits = PolicyLimits(
            rate_per_minute_global=int(limits_data.get("rate_per_minute_global", 120)),
            rate_per_minute_per_tool=int(limits_data.get("rate_per_minute_per_tool", 30)),
            request_size_kb=int(limits_data.get("request_size_kb", 512)),
            array_items_max=int(limits_data.get("array_items_max", 10_000)),
        )

        paths_data = data.get("paths", {}) if isinstance(data, dict) else {}
        paths = PathRules(
            allowed=[str(p) for p in paths_data.get("allowed", []) if isinstance(p, str)],
            forbidden=[str(p) for p in paths_data.get("forbidden", []) if isinstance(p, str)],
        )

        audit_data = data.get("audit", {}) if isinstance(data, dict) else {}
        audit = AuditRules(
            require_signature=bool(audit_data.get("require_signature", True)),
            hmac_secret_env=str(audit_data.get("hmac_secret_env", "MCP_AUDIT_SECRET")),
        )

        return Policy(roles=roles, limits=limits, paths=paths, audit=audit)


def normalize_patterns(patterns: Iterable[str]) -> List[str]:
    normalized: List[str] = []
    for pattern in patterns:
        pattern = str(pattern).strip()
        if pattern:
            normalized.append(pattern)
    return normalized


def evaluate_patterns(patterns: Iterable[str], target: str) -> bool:
    """Evaluate allow/deny patterns in order for *target*.

    Patterns prefixed with ``!`` act as explicit denies and take precedence.
    The evaluation returns ``True`` when there is at least one matching allow
    and no matching deny.
    """

    normalized_patterns = normalize_patterns(patterns)
    allowed = False
    for pattern in normalized_patterns:
        negate = pattern.startswith("!")
        candidate = pattern[1:].strip() if negate else pattern
        if not candidate:
            continue
        if fnmatch.fnmatchcase(target, candidate):
            if negate:
                return False
            allowed = True
    return allowed


__all__ = [
    "AuditRules",
    "PathRules",
    "Policy",
    "PolicyLimits",
    "PolicyLoader",
    "RoleRules",
    "evaluate_patterns",
]
