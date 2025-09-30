"""Schema helpers for recipe validation."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Set

__all__ = ["SchemaError", "ValidatedRecipe", "StepSpec", "validate_recipe", "select_recipe"]


class SchemaError(ValueError):
    """Raised when the recipe definition is invalid."""


@dataclass
class StepSpec:
    name: str
    tool: str
    params: Optional[dict]
    params_file: Optional[str]
    needs: List[str]
    raw: dict


@dataclass
class ValidatedRecipe:
    name: str
    version: int
    steps: List[StepSpec]
    vars: Dict[str, str]


def _ensure_unique_steps(steps: Iterable[StepSpec]) -> None:
    seen: Set[str] = set()
    for step in steps:
        if step.name in seen:
            raise SchemaError(f"Duplicate step name: {step.name}")
        seen.add(step.name)


def _validate_dependencies(steps: List[StepSpec]) -> None:
    step_names = {step.name for step in steps}
    for step in steps:
        for dep in step.needs:
            if dep not in step_names:
                raise SchemaError(f"Step '{step.name}' depends on unknown step '{dep}'")
    # Simple cycle detection using DFS
    graph = {step.name: set(step.needs) for step in steps}
    visiting: Set[str] = set()
    visited: Set[str] = set()

    def dfs(node: str) -> None:
        if node in visiting:
            raise SchemaError(f"Cycle detected involving step '{node}'")
        if node in visited:
            return
        visiting.add(node)
        for neighbor in graph[node]:
            dfs(neighbor)
        visiting.remove(node)
        visited.add(node)

    for step in steps:
        dfs(step.name)


def validate_recipe(recipe: dict, *, name: str = "default") -> ValidatedRecipe:
    if not isinstance(recipe, dict):
        raise SchemaError("Recipe definition must be a mapping")
    version = recipe.get("version")
    if version != 1:
        raise SchemaError("Only recipe version=1 is supported")
    raw_steps = recipe.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        raise SchemaError("Recipe must define at least one step")

    steps: List[StepSpec] = []
    for index, item in enumerate(raw_steps):
        if not isinstance(item, dict):
            raise SchemaError(f"Step at index {index} must be a mapping")
        step_name = item.get("name")
        tool_name = item.get("tool")
        params = item.get("params")
        params_file = item.get("params_file")
        if not isinstance(step_name, str) or not step_name:
            raise SchemaError(f"Step at index {index} is missing a valid name")
        if not isinstance(tool_name, str) or not tool_name:
            raise SchemaError(f"Step '{step_name}' must define a tool")
        if params is not None and params_file is not None:
            raise SchemaError(f"Step '{step_name}' cannot define both params and params_file")
        if params is None and params_file is None:
            raise SchemaError(f"Step '{step_name}' must define params or params_file")
        if params is not None and not isinstance(params, dict):
            raise SchemaError(f"Step '{step_name}' params must be a mapping")
        if params_file is not None and not isinstance(params_file, str):
            raise SchemaError(f"Step '{step_name}' params_file must be a string path")
        needs_raw = item.get("needs", [])
        if needs_raw is None:
            needs = []
        elif isinstance(needs_raw, list) and all(isinstance(x, str) for x in needs_raw):
            needs = list(needs_raw)
        else:
            raise SchemaError(f"Step '{step_name}' needs must be a list of step names")
        steps.append(StepSpec(step_name, tool_name, params, params_file, needs, dict(item)))

    _ensure_unique_steps(steps)
    _validate_dependencies(steps)

    vars_section = recipe.get("vars", {})
    if not isinstance(vars_section, dict):
        raise SchemaError("Recipe vars must be a mapping")
    cast_vars: Dict[str, str] = {}
    for key, value in vars_section.items():
        if not isinstance(key, str):
            raise SchemaError("Recipe variable names must be strings")
        cast_vars[key] = str(value)

    return ValidatedRecipe(name=name, version=version, steps=steps, vars=cast_vars)


def select_recipe(document: dict, *, recipe_name: Optional[str] = None) -> tuple[str, dict]:
    """Select the recipe definition from a YAML document."""

    if recipe_name:
        recipes = document.get("recipes")
        if not isinstance(recipes, dict):
            raise SchemaError("Document does not define named recipes")
        if recipe_name not in recipes:
            raise SchemaError(f"Recipe '{recipe_name}' not found in document")
        selected = recipes[recipe_name]
        return recipe_name, selected

    if "steps" in document:
        return recipe_name or "default", document

    recipes = document.get("recipes")
    if isinstance(recipes, dict) and len(recipes) == 1:
        (key, value), *_ = recipes.items()
        return key, value

    raise SchemaError("Unable to determine which recipe to execute; specify name with #recipe")
