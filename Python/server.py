"""Server-level tool registrations for the MCP server."""

from __future__ import annotations

from typing import Any, Dict

from mcp.server.fastmcp import Context, FastMCP

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


__all__ = ["register_server_tools"]

