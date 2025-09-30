"""Recipe loading and execution utilities."""

from __future__ import annotations

import json
import os
import re
import time
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import jmespath
import yaml
from tenacity import Retrying, retry_if_exception_type, stop_after_attempt, wait_exponential, wait_random

from .logs import get_logger
from .mcp_client import MCPClient, ProtocolError
from .schema import SchemaError, StepSpec, ValidatedRecipe, select_recipe, validate_recipe

__all__ = [
    "LoadedRecipe",
    "RecipeExecutor",
    "load_recipe_file",
    "merge_variables",
    "render_value",
]

logger = get_logger("recipes")


@dataclass
class LoadedRecipe:
    recipe: ValidatedRecipe
    source_path: Path
    document: dict


@dataclass
class StepResult:
    name: str
    ok: bool
    response: Dict[str, Any]
    duration: float
    skipped: bool = False
    saved_path: Optional[Path] = None


def load_recipe_file(path: Path, recipe_name: Optional[str] = None) -> LoadedRecipe:
    resolved_path = path.resolve()
    if not resolved_path.exists():
        raise FileNotFoundError(f"Recipe file not found: {resolved_path}")
    with resolved_path.open("r", encoding="utf-8") as handle:
        document = yaml.safe_load(handle) or {}
    if not isinstance(document, dict):
        raise SchemaError("Recipe file must contain a mapping at the top level")
    name, recipe_body = select_recipe(document, recipe_name=recipe_name)
    validated = validate_recipe(recipe_body, name=name)
    return LoadedRecipe(recipe=validated, source_path=resolved_path, document=document)


def merge_variables(
    recipe_vars: Dict[str, str],
    cli_vars: Dict[str, str],
    file_vars: Dict[str, Any],
) -> Dict[str, Any]:
    merged: Dict[str, Any] = {key: value for key, value in os.environ.items()}
    merged.update(recipe_vars)
    merged.update(file_vars)
    merged.update(cli_vars)
    return merged


_ENV_PATTERN = re.compile(r"\$\{([^{}]+)\}")
_STEP_PATTERN = re.compile(r"\$\{\{([^{}]+)\}\}")


def _resolve_env_expression(expr: str, variables: Dict[str, Any]) -> str:
    if ":-" in expr:
        key, default = expr.split(":-", 1)
        key = key.strip()
        default_value = default
    else:
        key, default_value = expr, None
    key = key.strip()
    if key in variables and variables[key] is not None:
        return str(variables[key])
    if key in os.environ:
        return os.environ[key]
    if default_value is not None:
        return str(default_value)
    return ""


def _replace_env_strings(value: str, variables: Dict[str, Any]) -> str:
    def repl(match: re.Match[str]) -> str:
        expr = match.group(1)
        return _resolve_env_expression(expr, variables)

    return _ENV_PATTERN.sub(repl, value)


def _replace_step_references(value: str, context: Dict[str, Any]) -> str:
    def repl(match: re.Match[str]) -> str:
        expression = match.group(1).strip()
        try:
            result = jmespath.search(expression, context)
        except Exception as exc:
            raise SchemaError(f"Failed to evaluate expression '{expression}': {exc}") from exc
        if result is None:
            return ""
        if isinstance(result, (dict, list)):
            return json.dumps(result)
        return str(result)

    return _STEP_PATTERN.sub(repl, value)


def render_value(value: Any, variables: Dict[str, Any], context: Dict[str, Any]) -> Any:
    if isinstance(value, str):
        replaced = _replace_step_references(value, context)
        replaced = _replace_env_strings(replaced, variables)
        return replaced
    if isinstance(value, list):
        return [render_value(item, variables, context) for item in value]
    if isinstance(value, dict):
        return {key: render_value(val, variables, context) for key, val in value.items()}
    return value


def _load_params_from_file(base_dir: Path, file_path: str) -> Dict[str, Any]:
    resolved_path = (base_dir / file_path).resolve()
    if not resolved_path.exists():
        raise FileNotFoundError(f"Params file not found: {resolved_path}")
    with resolved_path.open("r", encoding="utf-8") as handle:
        text = handle.read()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return yaml.safe_load(text)


def _evaluate_condition(value: Any, variables: Dict[str, Any], context: Dict[str, Any]) -> bool:
    if value is None:
        return True
    evaluated = render_value(value, variables, context)
    if isinstance(evaluated, bool):
        return evaluated
    if isinstance(evaluated, (int, float)):
        return bool(evaluated)
    if isinstance(evaluated, str):
        normalized = evaluated.strip().lower()
        if normalized in {"", "true", "1", "yes", "on"}:
            return True
        if normalized in {"false", "0", "no", "off"}:
            return False
        return bool(normalized)
    return bool(evaluated)


def _step_retry_config(step_raw: dict, default_attempts: int) -> Tuple[int, float, float]:
    retry_cfg = step_raw.get("retry")
    attempts = default_attempts
    backoff = 1.0
    jitter = 0.0
    if isinstance(retry_cfg, dict):
        max_attempts = retry_cfg.get("max_attempts")
        if isinstance(max_attempts, int) and max_attempts > 0:
            attempts = max_attempts
        backoff_val = retry_cfg.get("backoff_sec")
        if isinstance(backoff_val, (int, float)) and backoff_val > 0:
            backoff = float(backoff_val)
        jitter_val = retry_cfg.get("jitter")
        if isinstance(jitter_val, (int, float)) and jitter_val >= 0:
            jitter = float(jitter_val)
    return attempts, backoff, jitter


class RecipeExecutor:
    def __init__(
        self,
        client: MCPClient,
        loaded: LoadedRecipe,
        *,
        variables: Dict[str, Any],
    ) -> None:
        self.client = client
        self.loaded = loaded
        self.variables = variables
        self.context: Dict[str, Any] = {"steps": {}, "vars": variables}
        self.base_dir = loaded.source_path.parent
        self._step_map: Dict[str, StepSpec] = {step.name: step for step in loaded.recipe.steps}

    def plan(self) -> List[str]:
        from graphlib import TopologicalSorter

        sorter = TopologicalSorter()
        for step in self.loaded.recipe.steps:
            sorter.add(step.name, *step.needs)
        return list(sorter.static_order())

    def execute(
        self,
        *,
        dry_run: bool = False,
        default_retry: int = 1,
        parallelism: int = 1,
        continue_on_error: bool = False,
        default_timeout: Optional[float] = None,
        allow_save: bool = True,
    ) -> Dict[str, Any]:
        from graphlib import TopologicalSorter

        sorter = TopologicalSorter()
        for step in self.loaded.recipe.steps:
            sorter.add(step.name, *step.needs)
        sorter.prepare()

        executor = ThreadPoolExecutor(max_workers=max(1, parallelism))
        futures: Dict[Future[StepResult], str] = {}
        results: Dict[str, StepResult] = {}
        failed = False
        audit_actions: List[Any] = []
        audit_diffs: List[Any] = []

        try:
            while True:
                ready = list(sorter.get_ready())
                if not ready and not futures:
                    break
                for name in ready:
                    step_spec = self._step_map[name]
                    step_context = self.context
                    condition = _evaluate_condition(step_spec.raw.get("when"), self.variables, step_context)
                    if not condition:
                        logger.info("Skipping step %s (condition false)", name)
                        result = StepResult(name=name, ok=True, response={}, duration=0.0, skipped=True)
                        results[name] = result
                        self.context["steps"][name] = {"skipped": True, "ok": True, "result": {}}
                        sorter.done(name)
                        continue
                    dependency_states = [results[dep] for dep in step_spec.needs if dep in results]
                    if any(not dep_result.ok for dep_result in dependency_states):
                        logger.warning("Skipping step %s because dependency failed", name)
                        result = StepResult(name=name, ok=False, response={}, duration=0.0, skipped=True)
                        results[name] = result
                        self.context["steps"][name] = {"skipped": True, "ok": False, "result": {}}
                        failed = True
                        sorter.done(name)
                        continue

                    future = executor.submit(
                        self._run_step,
                        step_spec,
                        dry_run,
                        default_retry,
                        default_timeout,
                        allow_save,
                    )
                    futures[future] = name

                if not futures:
                    continue

                done, _ = wait(futures.keys(), return_when=FIRST_COMPLETED)
                abort = False
                for future in done:
                    name = futures.pop(future)
                    try:
                        step_result = future.result()
                    except Exception as exc:  # pragma: no cover - defensive
                        logger.exception("Step %s raised an unexpected error", name)
                        step_result = StepResult(name=name, ok=False, response={"ok": False, "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}, duration=0.0)
                    results[name] = step_result
                    self.context["steps"][name] = {
                        "ok": step_result.ok,
                        "skipped": step_result.skipped,
                        "result": step_result.response,
                    }
                    sorter.done(name)
                    if not step_result.ok and not step_result.skipped:
                        failed = True
                        if not continue_on_error:
                            logger.error("Step %s failed; aborting remaining steps", name)
                            for pending in futures:
                                pending.cancel()
                            futures.clear()
                            abort = True
                            break
                        logger.warning("Continuing despite failure in %s", name)
                    audit = step_result.response.get("audit") if isinstance(step_result.response, dict) else None
                    if isinstance(audit, dict):
                        actions = audit.get("actions")
                        if isinstance(actions, list):
                            audit_actions.extend(actions)
                        diffs = audit.get("diffs")
                        if isinstance(diffs, list):
                            audit_diffs.extend(diffs)
                if abort:
                    break
        finally:
            executor.shutdown(wait=True)

        for name in self._step_map:
            if name not in results:
                results[name] = StepResult(name=name, ok=False, response={}, duration=0.0, skipped=True)
                failed = True

        summary = {
            "ok": not failed,
            "steps": {
                name: {
                    "ok": result.ok,
                    "skipped": result.skipped,
                    "durSec": round(result.duration, 3),
                    **({"save_as": str(result.saved_path)} if result.saved_path else {}),
                }
                for name, result in results.items()
            },
            "audit": {
                "actions": audit_actions,
                "diffs": audit_diffs,
            },
            "plan": self.plan(),
        }
        return summary

    def _run_step(
        self,
        step: StepSpec,
        dry_run: bool,
        default_retry: int,
        default_timeout: Optional[float],
        allow_save: bool,
    ) -> StepResult:
        params: Any = step.params
        path_value = step.params_file
        if path_value:
            rendered_path = render_value(path_value, self.variables, self.context)
            if not isinstance(rendered_path, str):
                raise SchemaError(f"Step {step.name} params_file must resolve to a string path")
            params = _load_params_from_file(self.base_dir, rendered_path)
        params = render_value(params, self.variables, self.context) if params is not None else {}
        if dry_run and isinstance(params, dict) and "DryRun" not in params:
            params = dict(params)
            params.setdefault("DryRun", True)
        meta = {
            "step": step.name,
            "dryRun": dry_run,
            "vars": {k: v for k, v in self.variables.items() if isinstance(k, str)},
        }
        timeout = default_timeout
        timeout_raw = step.raw.get("timeout_sec")
        if isinstance(timeout_raw, (int, float)) and timeout_raw > 0:
            timeout = float(timeout_raw)

        attempts, backoff, jitter = _step_retry_config(step.raw, default_retry)

        start = time.time()

        def invoke() -> Dict[str, Any]:
            return self.client.call_tool(step.tool, params, meta=meta, timeout=timeout)

        try:
            if attempts <= 1:
                response = invoke()
            else:
                waits = wait_exponential(multiplier=backoff, min=backoff, max=max(backoff * 4, backoff))
                if jitter > 0:
                    waits = waits + wait_random(0, jitter)
                retry = Retrying(
                    retry=retry_if_exception_type((ProtocolError, OSError)),
                    stop=stop_after_attempt(attempts),
                    wait=waits,
                    reraise=True,
                )
                for attempt in retry:
                    with attempt:
                        response = invoke()
                        break
            duration = time.time() - start
            ok = bool(response.get("ok", False)) if isinstance(response, dict) else False
            saved_path = None
            save_as = step.raw.get("save_as")
            if allow_save and isinstance(save_as, str):
                save_path = (self.base_dir / save_as).resolve()
                save_path.parent.mkdir(parents=True, exist_ok=True)
                with save_path.open("w", encoding="utf-8") as handle:
                    json.dump(response, handle, indent=2)
                saved_path = save_path
            return StepResult(name=step.name, ok=ok, response=response, duration=duration, saved_path=saved_path)
        except Exception as exc:
            duration = time.time() - start
            logger.error("Step %s raised exception: %s", step.name, exc)
            return StepResult(
                name=step.name,
                ok=False,
                response={"ok": False, "error": {"code": "EXCEPTION", "message": str(exc)}},
                duration=duration,
            )
