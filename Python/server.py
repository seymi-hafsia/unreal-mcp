"""Server-level tool registrations for the MCP server."""

from __future__ import annotations

import json
import platform
import time
import uuid
from datetime import datetime, timezone
from typing import Any, Dict, Optional

from mcp.server.fastmcp import Context, FastMCP

from automation_specs import run_specs
from gauntlet import run_gauntlet
from uat import run_buildcookrun
from security.audit_sign import AuditRecord, AuditSigner, load_secret_from_env
from security.policy import PolicyLoader
from security.rate_limit import RateLimitConfig, RateLimiter
from security.sandbox import is_path_allowed
from security.schema_registry import validate as validate_schema


_POLICY_LOADER = PolicyLoader()
_CURRENT_POLICY = _POLICY_LOADER.load()
_RATE_LIMITER = RateLimiter(
    RateLimitConfig(
        per_minute_global=_CURRENT_POLICY.limits.rate_per_minute_global,
        per_minute_tool=_CURRENT_POLICY.limits.rate_per_minute_per_tool,
    )
)
_AUDIT_SECRET_VALUE = (
    load_secret_from_env(_CURRENT_POLICY.audit.hmac_secret_env)
    if _CURRENT_POLICY.audit.require_signature
    else None
)
_AUDIT_SIGNER = AuditSigner(_AUDIT_SECRET_VALUE)


def _refresh_policy() -> None:
    global _CURRENT_POLICY, _RATE_LIMITER, _AUDIT_SIGNER, _AUDIT_SECRET_VALUE
    policy = _POLICY_LOADER.load()
    if (
        policy.limits.rate_per_minute_global != _CURRENT_POLICY.limits.rate_per_minute_global
        or policy.limits.rate_per_minute_per_tool != _CURRENT_POLICY.limits.rate_per_minute_per_tool
    ):
        _RATE_LIMITER = RateLimiter(
            RateLimitConfig(
                per_minute_global=policy.limits.rate_per_minute_global,
                per_minute_tool=policy.limits.rate_per_minute_per_tool,
            )
        )
    expected_secret = (
        load_secret_from_env(policy.audit.hmac_secret_env) if policy.audit.require_signature else None
    )
    if expected_secret != _AUDIT_SECRET_VALUE:
        _AUDIT_SECRET_VALUE = expected_secret
        _AUDIT_SIGNER = AuditSigner(expected_secret)
    elif not policy.audit.require_signature and _AUDIT_SECRET_VALUE is not None:
        _AUDIT_SECRET_VALUE = None
        _AUDIT_SIGNER = AuditSigner(None)
    _CURRENT_POLICY = policy


def _get_policy():
    _refresh_policy()
    return _CURRENT_POLICY


def _extract_role(ctx: Context, policy) -> str:
    default_role = "dev"
    role = default_role
    session = getattr(ctx, "session", None)
    if isinstance(session, dict):
        candidate = session.get("role")
        if isinstance(candidate, str):
            role = candidate
    meta = getattr(ctx, "meta", None)
    if isinstance(meta, dict):
        candidate = meta.get("role")
        if isinstance(candidate, str):
            role = candidate
    if role not in policy.roles:
        return default_role
    return role


def _flatten_paths(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, (list, tuple)):
        for item in value:
            yield from _flatten_paths(item)
    elif isinstance(value, dict):
        for item in value.values():
            yield from _flatten_paths(item)


def _iter_paths(params: Dict[str, Any]):
    for key, value in params.items():
        key_lower = key.lower()
        if any(token in key_lower for token in ("path", "dir", "root")):
            yield from _flatten_paths(value)


def _max_array_length(payload: Dict[str, Any]) -> int:
    max_len = 0
    stack = [payload]
    while stack:
        current = stack.pop()
        if isinstance(current, dict):
            stack.extend(current.values())
        elif isinstance(current, list):
            max_len = max(max_len, len(current))
            stack.extend(current)
    return max_len


def _redact_sensitive(value: Any, key: Optional[str] = None) -> Any:
    sensitive_markers = {"token", "password", "secret", "key"}
    if isinstance(value, dict):
        return {k: _redact_sensitive(v, k) for k, v in value.items()}
    if isinstance(value, list):
        return [_redact_sensitive(item, key) for item in value]
    if isinstance(value, str) and (key and any(marker in key.lower() for marker in sensitive_markers)):
        return "[REDACTED]"
    if isinstance(value, str) and any(marker in value.lower() for marker in sensitive_markers):
        return "[REDACTED]"
    return value


def _apply_security(ctx: Context, tool_name: str, params: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    policy = _get_policy()
    role = _extract_role(ctx, policy)
    if not policy.is_tool_allowed(role, tool_name):
        return {
            "ok": False,
            "error": {"code": "TOOL_DENIED", "message": "Denied by role/policy", "details": {"role": role}},
        }

    allowed, retry = _RATE_LIMITER.check(tool_name)
    if not allowed:
        return {
            "ok": False,
            "error": {
                "code": "RATE_LIMITED",
                "message": "Rate limit exceeded",
                "details": {"retryAfterSec": int(retry) + 1},
            },
        }

    serialized = json.dumps(params or {}, separators=(",", ":"), ensure_ascii=False)
    if len(serialized.encode("utf-8")) > policy.limits.request_size_kb * 1024:
        return {
            "ok": False,
            "error": {"code": "REQUEST_TOO_LARGE", "message": "Request exceeds configured size limit"},
        }

    if _max_array_length(params) > policy.limits.array_items_max:
        return {
            "ok": False,
            "error": {"code": "ARRAY_TOO_LARGE", "message": "Array exceeds configured item limit"},
        }

    schema_error = validate_schema(tool_name, params)
    if schema_error:
        return {
            "ok": False,
            "error": {"code": "INVALID_PARAMS", "message": "Schema validation failed", "details": {"error": schema_error}},
        }

    for candidate in _iter_paths(params):
        if isinstance(candidate, str) and candidate:
            if not is_path_allowed(candidate, policy.paths.allowed, policy.paths.forbidden):
                return {
                    "ok": False,
                    "error": {
                        "code": "PATH_NOT_ALLOWED",
                        "message": "Path outside sandbox",
                        "details": {"path": candidate},
                    },
                }

    return None


def _finalize_response(tool_name: str, ctx: Context, params: Dict[str, Any], response: Dict[str, Any]) -> Dict[str, Any]:
    if not isinstance(response, dict):
        return response
    policy = _get_policy()
    result = dict(response)
    if policy.audit.require_signature and _AUDIT_SIGNER and response.get("ok"):
        request_id = getattr(ctx, "request_id", None) or getattr(ctx, "requestId", None) or str(uuid.uuid4())
        audit_payload = {
            "params": _redact_sensitive(params),
            "result": _redact_sensitive(response.get("result", {}), "result"),
            "audit": _redact_sensitive(response.get("audit", {}), "audit"),
        }
        signature = _AUDIT_SIGNER.sign(AuditRecord(request_id=request_id, tool=tool_name, payload=audit_payload))
        if signature:
            result.setdefault("security", {}).update(signature)
    return result


def register_server_tools(mcp: FastMCP) -> None:
    """Register tools exposed directly by the Python server."""

    @mcp.tool(name="uat.buildcookrun")
    def _uat_buildcookrun(ctx: Context, params: Dict[str, Any]) -> Dict[str, Any]:
        """Invoke RunUAT BuildCookRun via the Python wrapper."""

        if params is None:
            payload: Dict[str, Any] = {}
        elif isinstance(params, dict):
            payload = params
        else:
            return {
                "ok": False,
                "error": {
                    "code": "INVALID_PARAMS",
                    "message": "Expected params to be an object for uat.buildcookrun.",
                    "details": {"receivedType": type(params).__name__},
                },
            }
        violation = _apply_security(ctx, "uat.buildcookrun", payload)
        if violation:
            return violation

        return _finalize_response("uat.buildcookrun", ctx, payload, run_buildcookrun(payload))

    @mcp.tool(name="automation.run_specs")
    def _automation_run_specs(ctx: Context, params: Dict[str, Any]) -> Dict[str, Any]:
        """Run Unreal Automation Tests via UnrealEditor-Cmd."""

        if params is None:
            payload = {}
        elif isinstance(params, dict):
            payload = params
        else:
            return {
                "ok": False,
                "error": {
                    "code": "INVALID_PARAMS",
                    "message": "Expected params to be an object for automation.run_specs.",
                    "details": {"receivedType": type(params).__name__},
                },
            }
        violation = _apply_security(ctx, "automation.run_specs", payload)
        if violation:
            return violation

        return _finalize_response("automation.run_specs", ctx, payload, run_specs(payload))

    @mcp.tool(name="gauntlet.run")
    def _gauntlet_run(ctx: Context, params: Dict[str, Any]) -> Dict[str, Any]:
        """Run a Gauntlet suite via RunUAT."""

        if params is None:
            payload = {}
        elif isinstance(params, dict):
            payload = params
        else:
            return {
                "ok": False,
                "error": {
                    "code": "INVALID_PARAMS",
                    "message": "Expected params to be an object for gauntlet.run.",
                    "details": {"receivedType": type(params).__name__},
                },
            }
        violation = _apply_security(ctx, "gauntlet.run", payload)
        if violation:
            return violation

        return _finalize_response("gauntlet.run", ctx, payload, run_gauntlet(payload))

    @mcp.tool(name="mcp.health")
    def _mcp_health(ctx: Context, params: Dict[str, Any]) -> Dict[str, Any]:
        """Return server health, configuration, and plugin state."""

        from unreal_mcp_server import (  # local import to avoid cycles
            SERVER_CONFIG,
            SERVER_IDENTITY,
            SERVER_START_TIME,
            UnrealConnection,
            get_unreal_connection,
        )

        connection = get_unreal_connection()
        uptime_sec = time.time() - SERVER_START_TIME
        clients = 1 if connection and connection.connected else 0
        rtt_ms: Optional[float] = None
        if connection:
            try:
                ping_response = connection.send_command("ping", {})
                if isinstance(ping_response, dict):
                    meta = ping_response.get("meta") or {}
                    if isinstance(meta, dict):
                        rtt_val = meta.get("durMs")
                        if isinstance(rtt_val, (int, float)):
                            rtt_ms = float(rtt_val)
            except Exception:  # pragma: no cover - best-effort RTT
                rtt_ms = None

        enforcement = {
            "allowWrite": SERVER_CONFIG.allow_write,
            "dryRun": SERVER_CONFIG.dry_run,
            "allowedPaths": SERVER_CONFIG.normalized_paths(),
        }

        plugin_info: Dict[str, Any] = {}
        if isinstance(connection, UnrealConnection):
            if connection.last_handshake:
                plugin_info["lastHandshake"] = connection.last_handshake.isoformat()
            if connection.remote_engine_version:
                plugin_info["engineVersion"] = connection.remote_engine_version
            if connection.remote_plugin_version:
                plugin_info["pluginVersion"] = connection.remote_plugin_version

        server_info = {
            "version": SERVER_IDENTITY,
            "protocolVersion": UnrealConnection.PROTOCOL_VERSION,
            "python": platform.python_version(),
            "platform": platform.platform(),
            "uptimeSec": int(uptime_sec),
            "clients": clients,
        }

        return {
            "ok": True,
            "server": server_info,
            "enforcement": enforcement,
            "rttMs": rtt_ms,
            "plugin": plugin_info,
        }

__all__ = ["register_server_tools"]

