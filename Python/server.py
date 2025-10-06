# Python/server.py  — squelette FastMCP minimal
from mcp.server.fastmcp import FastMCP, Context
import platform, time, os

mcp = FastMCP("unreal-mcp-fork")

@mcp.tool()
def mcp_health(ctx: Context) -> dict:
    """Health check simple (read-only)."""
    return {
        "ok": True,
        "server": {
            "name": "unreal-mcp-fork",
            "python": platform.python_version(),
            "platform": platform.platform(),
            "cwd": os.getcwd(),
        },
        "time": time.time(),
        "env": { "UE_ENGINE_ROOT": os.environ.get("UE_ENGINE_ROOT") },
    }

if __name__ == "__main__":
    # Compat multi-versions du package `mcp`
    run = getattr(mcp, "run_stdio", None) or getattr(mcp, "run", None)
    if run is None:
        raise RuntimeError("FastMCP: ni run_stdio ni run() — mets à jour le paquet 'mcp'.")
    run()

