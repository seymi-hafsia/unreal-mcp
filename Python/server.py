"""Server-level tool registrations for the MCP server."""

from __future__ import annotations

import platform
import time
from typing import Any, Dict, Optional

from mcp.server.fastmcp import Context, FastMCP

from automation_specs import run_specs
from gauntlet import run_gauntlet
from uat import run_buildcookrun


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

        return run_buildcookrun(payload)

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

        return run_specs(payload)

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

        return run_gauntlet(payload)

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

