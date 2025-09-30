"""Entry point for the ``mcp`` command line interface."""

from __future__ import annotations

from mcp_cli.cli import app


def main() -> None:
    """Execute the Typer application."""
    app()


if __name__ == "__main__":  # pragma: no cover - manual execution
    main()
