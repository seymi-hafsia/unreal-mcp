"""Server-level tool registrations for the MCP server."""

from __future__ import annotations

from typing import Any, Dict

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

__all__ = ["register_server_tools"]

