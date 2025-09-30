"""Command line interface implementation for the MCP client."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import jmespath
import typer
import yaml

from .logs import configure_logging, get_logger
from .mcp_client import MCPClient, ProtocolError
from .recipes import (
    LoadedRecipe,
    RecipeExecutor,
    load_recipe_file,
    merge_variables,
    render_value,
)
from .schema import SchemaError

app = typer.Typer(help="Interact with the Unreal MCP server from the command line.")
recipe_app = typer.Typer(help="Work with pipeline recipes defined in YAML files.")
app.add_typer(recipe_app, name="recipe")


def _parse_key_values(pairs: List[str], *, option: str) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for item in pairs:
        if "=" not in item:
            raise typer.BadParameter(f"Expected KEY=VALUE format for {option}")
        key, value = item.split("=", 1)
        if not key:
            raise typer.BadParameter(f"Invalid key for {option}")
        values[key] = value
    return values


def _load_vars_file(path: Optional[Path]) -> Dict[str, Any]:
    if not path:
        return {}
    resolved = path.expanduser().resolve()
    if not resolved.exists():
        raise typer.BadParameter(f"Vars file not found: {resolved}")
    with resolved.open("r", encoding="utf-8") as handle:
        text = handle.read()
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        data = yaml.safe_load(text)
    if not isinstance(data, dict):
        raise typer.BadParameter("Vars file must contain a mapping")
    return data


def _load_params_source(params_json: Optional[str], params_file: Optional[Path]) -> Any:
    payload: Any = None
    if params_file:
        resolved = params_file.expanduser().resolve()
        if not resolved.exists():
            raise typer.BadParameter(f"Params file not found: {resolved}")
        with resolved.open("r", encoding="utf-8") as handle:
            text = handle.read()
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = yaml.safe_load(text)
    elif params_json:
        try:
            payload = json.loads(params_json)
        except json.JSONDecodeError as exc:
            raise typer.BadParameter(f"Invalid JSON for --params-json: {exc}") from exc
    if payload is None:
        payload = {}
    return payload


def _apply_env_overrides(overrides: Dict[str, str]) -> None:
    for key, value in overrides.items():
        os.environ[key] = value


def _format_output(data: Any, *, output: str) -> str:
    fmt = output.lower()
    if fmt not in {"json", "yaml"}:
        raise typer.BadParameter("--output must be either 'json' or 'yaml'")
    if fmt == "yaml":
        return yaml.safe_dump(data, sort_keys=False)
    return json.dumps(data, indent=2, ensure_ascii=False)


def _apply_select(data: Any, select: Optional[str]) -> Any:
    if not select:
        return data
    return jmespath.search(select, data)


def _configure_and_get_logger(level: str, name: str):
    configure_logging(level)
    return get_logger(name)


def _parse_recipe_reference(reference: str) -> Tuple[Path, Optional[str]]:
    if "#" in reference:
        path_part, recipe_name = reference.split("#", 1)
        return Path(path_part), recipe_name or None
    return Path(reference), None


def _build_recipe(
    recipe_path: Path,
    recipe_name: Optional[str],
) -> LoadedRecipe:
    try:
        return load_recipe_file(recipe_path, recipe_name)
    except (SchemaError, FileNotFoundError) as exc:
        raise typer.BadParameter(str(exc)) from exc


@app.command("run")
def run_tool(
    tool: str = typer.Argument(..., help="Name of the MCP tool to execute."),
    params_json: Optional[str] = typer.Option(None, "--params-json", help="Inline JSON parameters."),
    params_file: Optional[Path] = typer.Option(None, "--params-file", help="Path to a JSON/YAML file with parameters."),
    server: str = typer.Option("127.0.0.1:8765", "--server", envvar="MCP_SERVER", help="Address of the MCP server."),
    dry_run: bool = typer.Option(False, "--dry-run", help="Mark the invocation as a dry-run."),
    retry: int = typer.Option(1, "--retry", min=1, help="Number of retry attempts on transport errors."),
    timeout: Optional[float] = typer.Option(None, "--timeout", help="Seconds to wait for tool response."),
    vars_inline: List[str] = typer.Option([], "--vars", help="Variables to substitute (KEY=VALUE)."),
    vars_file: Optional[Path] = typer.Option(None, "--vars-file", help="YAML/JSON file containing variables."),
    env_overrides: List[str] = typer.Option([], "--env", help="Environment overrides (KEY=VALUE)."),
    select: Optional[str] = typer.Option(None, "--select", help="JMESPath expression to filter the result."),
    output: str = typer.Option("json", "--output", case_sensitive=False, help="Output format: json or yaml."),
    log_level: str = typer.Option("info", "--log-level", help="Logging level."),
) -> None:
    logger = _configure_and_get_logger(log_level, "command.run")
    env_vars = _parse_key_values(env_overrides, option="--env")
    _apply_env_overrides(env_vars)

    cli_vars = _parse_key_values(vars_inline, option="--vars")
    file_vars = _load_vars_file(vars_file)
    variables = merge_variables({}, cli_vars, file_vars)
    context = {"vars": variables, "steps": {}}

    params = _load_params_source(params_json, params_file)
    params = render_value(params, variables, context) if params is not None else {}
    if dry_run and isinstance(params, dict) and "DryRun" not in params:
        params = dict(params)
        params.setdefault("DryRun", True)

    meta = {"dryRun": dry_run, "vars": variables}
    report: Dict[str, Any]
    client = MCPClient(server)
    try:
        response = client.call_with_retry(
            tool,
            params,
            meta=meta,
            attempts=retry,
            timeout=timeout,
        )
    except ProtocolError as exc:
        logger.error("Transport error: %s", exc)
        raise typer.Exit(1) from exc
    except OSError as exc:
        logger.error("Socket error: %s", exc)
        raise typer.Exit(1) from exc
    finally:
        client.close()

    selected = _apply_select(response, select)
    print(_format_output(selected, output=output.lower()))

    ok = bool(response.get("ok", False)) if isinstance(response, dict) else False
    raise typer.Exit(0 if ok else 1)


@recipe_app.command("run")
def run_recipe(
    recipe: str = typer.Argument(..., help="Path to the recipe YAML file (optionally with #recipe)."),
    server: str = typer.Option("127.0.0.1:8765", "--server", envvar="MCP_SERVER", help="Address of the MCP server."),
    dry_run: bool = typer.Option(False, "--dry-run", help="Invoke each step with DryRun=true when possible."),
    retry: int = typer.Option(1, "--retry", min=1, help="Default retry attempts per step."),
    parallel: int = typer.Option(1, "--parallel", min=1, help="Maximum parallel steps to execute."),
    continue_on_error: bool = typer.Option(False, "--continue-on-error", help="Continue executing steps after failures."),
    timeout: Optional[float] = typer.Option(None, "--timeout", help="Default timeout (seconds) for each step."),
    vars_inline: List[str] = typer.Option([], "--vars", help="Variables to inject (KEY=VALUE)."),
    vars_file: Optional[Path] = typer.Option(None, "--vars-file", help="YAML/JSON file containing variables."),
    env_overrides: List[str] = typer.Option([], "--env", help="Environment overrides (KEY=VALUE)."),
    select: Optional[str] = typer.Option(None, "--select", help="JMESPath expression to filter the summary."),
    output: str = typer.Option("json", "--output", case_sensitive=False, help="Output format: json or yaml."),
    log_level: str = typer.Option("info", "--log-level", help="Logging level."),
) -> None:
    logger = _configure_and_get_logger(log_level, "command.recipe.run")
    env_vars = _parse_key_values(env_overrides, option="--env")
    _apply_env_overrides(env_vars)

    recipe_path, recipe_name = _parse_recipe_reference(recipe)
    loaded = _build_recipe(recipe_path, recipe_name)

    cli_vars = _parse_key_values(vars_inline, option="--vars")
    file_vars = _load_vars_file(vars_file)
    variables = merge_variables(loaded.recipe.vars, cli_vars, file_vars)

    client = MCPClient(server)
    executor = RecipeExecutor(client, loaded, variables=variables)
    plan = executor.plan()
    logger.info("Plan: %s", " -> ".join(plan))

    try:
        summary = executor.execute(
            dry_run=dry_run,
            default_retry=retry,
            parallelism=parallel,
            continue_on_error=continue_on_error,
            default_timeout=timeout,
            allow_save=True,
        )
    finally:
        client.close()

    selected = _apply_select(summary, select)
    print(_format_output(selected, output=output.lower()))

    raise typer.Exit(0 if summary.get("ok") else 1)


@recipe_app.command("test")
def test_recipe(
    recipe: str = typer.Argument(..., help="Path to the recipe YAML file (optionally with #recipe)."),
    server: str = typer.Option("127.0.0.1:8765", "--server", envvar="MCP_SERVER", help="Address of the MCP server."),
    dry_run: bool = typer.Option(False, "--dry-run", help="Execute steps in dry-run mode to collect audits."),
    retry: int = typer.Option(1, "--retry", min=1, help="Default retry attempts per step."),
    parallel: int = typer.Option(1, "--parallel", min=1, help="Maximum parallel steps to execute."),
    timeout: Optional[float] = typer.Option(None, "--timeout", help="Default timeout (seconds) for each step."),
    vars_inline: List[str] = typer.Option([], "--vars", help="Variables to inject (KEY=VALUE)."),
    vars_file: Optional[Path] = typer.Option(None, "--vars-file", help="YAML/JSON file containing variables."),
    env_overrides: List[str] = typer.Option([], "--env", help="Environment overrides (KEY=VALUE)."),
    select: Optional[str] = typer.Option(None, "--select", help="JMESPath expression to filter the test report."),
    output: str = typer.Option("json", "--output", case_sensitive=False, help="Output format: json or yaml."),
    log_level: str = typer.Option("info", "--log-level", help="Logging level."),
) -> None:
    _ = server  # server kept for parity; only used when dry-run executes steps.
    logger = _configure_and_get_logger(log_level, "command.recipe.test")
    env_vars = _parse_key_values(env_overrides, option="--env")
    _apply_env_overrides(env_vars)

    recipe_path, recipe_name = _parse_recipe_reference(recipe)
    loaded = _build_recipe(recipe_path, recipe_name)

    cli_vars = _parse_key_values(vars_inline, option="--vars")
    file_vars = _load_vars_file(vars_file)
    variables = merge_variables(loaded.recipe.vars, cli_vars, file_vars)

    client = MCPClient(server)
    try:
        plan_executor = RecipeExecutor(client, loaded, variables=variables)
        plan = plan_executor.plan()

        report: Dict[str, Any] = {
            "recipe": loaded.recipe.name,
            "plan": plan,
            "vars": variables,
        }

        if dry_run:
            executor = RecipeExecutor(client, loaded, variables=variables)
            logger.info("Executing dry-run against MCP server %s", server)
            summary = executor.execute(
                dry_run=True,
                default_retry=retry,
                parallelism=parallel,
                continue_on_error=True,
                default_timeout=timeout,
                allow_save=False,
            )
            report["dryRun"] = summary
            report["ok"] = summary.get("ok", False)
        else:
            report["ok"] = True
    finally:
        client.close()

    selected = _apply_select(report, select)
    print(_format_output(selected, output=output.lower()))

    raise typer.Exit(0 if report.get("ok") else 1)
