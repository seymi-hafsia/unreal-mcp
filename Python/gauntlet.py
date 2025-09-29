"""Implementation of the `gauntlet.run` MCP tool."""

from __future__ import annotations

import os
import platform
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from automation import (
    ProcessResult,
    ToolError,
    format_command_for_display,
    make_log_path,
    spawn_process,
)

LOG_ROOT = Path("logs") / "gauntlet"

RESULT_RE = re.compile(r"Results?\s*-\s*Total:\s*(\d+),\s*Passed:\s*(\d+),\s*Failed:\s*(\d+)", re.IGNORECASE)
GAUNTLET_LOG_PATTERNS = [
    re.compile(r"Gauntlet\s*(?:Log|log file)[:=]\s*(.+)", re.IGNORECASE),
    re.compile(r"Log file:\s*(.+Gauntlet.+\.log)", re.IGNORECASE),
]
ARTIFACT_PATTERNS = [
    re.compile(r"Artifacts?\s*(?:Dir|Directory|stored at|saved to)[:=]\s*(.+)", re.IGNORECASE),
]


@dataclass
class GauntletConfig:
    """Configuration for a Gauntlet run."""

    engine_root: Path
    runuat_path: Path
    uproject: Path
    test_name: str
    platform_name: str
    client_config: str
    build_path: Path
    timeout_seconds: Optional[int]
    extra_args: List[str]
    env: Dict[str, str]


def _parse_timeout(payload: Dict[str, Any]) -> Optional[int]:
    timeout_minutes = payload.get("timeoutMinutes")
    if timeout_minutes is None:
        return None
    try:
        timeout_value = float(timeout_minutes)
    except (TypeError, ValueError):
        raise ToolError("INVALID_TIMEOUT", "timeoutMinutes must be numeric.")
    if timeout_value <= 0:
        return None
    return int(timeout_value * 60)


def _normalize_string_sequence(value: Any, *, default: Optional[Sequence[str]] = None) -> List[str]:
    if value is None:
        return list(default or [])
    if isinstance(value, (list, tuple, set)):
        return [str(item) for item in value]
    return [str(value)]


def _ensure_path(path_str: Any, *, code: str, what: str) -> Path:
    if not path_str:
        raise ToolError(code, f"{what} is required.")
    candidate = Path(str(path_str)).expanduser().resolve()
    if not candidate.exists():
        raise ToolError(code, f"{what} does not exist.", {what: str(candidate)})
    return candidate


def _locate_runuat(engine_root: Path) -> Path:
    binaries_dir = engine_root / "Engine" / "Build" / "BatchFiles"
    system_name = platform.system().lower()
    if system_name.startswith("windows"):
        candidate = binaries_dir / "RunUAT.bat"
    else:
        candidate = binaries_dir / "RunUAT.sh"
    if not candidate.exists():
        raise ToolError(
            "ENGINE_NOT_FOUND",
            "RunUAT script not found under engineRoot.",
            {"expected": str(candidate)},
        )
    return candidate


def _clean_path(value: str) -> str:
    cleaned = value.strip().strip('"').strip("'")
    return cleaned


def _parse_gauntlet_log(log_path: Path) -> Tuple[Dict[str, Any], bool, Optional[str]]:
    total: Optional[int] = None
    passed: Optional[int] = None
    failed: Optional[int] = None
    artifacts_dir: Optional[str] = None
    gauntlet_log: Optional[str] = None

    with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            if total is None or passed is None or failed is None:
                match = RESULT_RE.search(line)
                if match:
                    total = int(match.group(1))
                    passed = int(match.group(2))
                    failed = int(match.group(3))
            if artifacts_dir is None:
                for pattern in ARTIFACT_PATTERNS:
                    match = pattern.search(line)
                    if match:
                        artifacts_dir = _clean_path(match.group(1))
                        break
            if gauntlet_log is None:
                for pattern in GAUNTLET_LOG_PATTERNS:
                    match = pattern.search(line)
                    if match:
                        gauntlet_log = _clean_path(match.group(1))
                        break

    parse_error = total is None or passed is None or failed is None
    results: Dict[str, Any] = {
        "total": total or 0,
        "passed": passed or 0,
        "failed": failed or 0,
    }
    if artifacts_dir:
        results["artifactsDir"] = artifacts_dir

    return results, parse_error, gauntlet_log


def _parse_payload(payload: Optional[Dict[str, Any]]) -> GauntletConfig:
    payload = payload or {}
    if not isinstance(payload, dict):
        raise ToolError("INVALID_PARAMS", "Expected params to be an object.")

    engine_root = _ensure_path(payload.get("engineRoot") or os.getenv("UE_ENGINE_ROOT"), code="ENGINE_NOT_FOUND", what="engineRoot")
    runuat_path = _locate_runuat(engine_root)

    uproject = _ensure_path(payload.get("uproject"), code="UPROJECT_NOT_FOUND", what="uproject")

    test_name = payload.get("test")
    if not test_name:
        raise ToolError("INVALID_PARAMS", "test is required (Gauntlet suite name).")
    test_name = str(test_name)

    platform_name = payload.get("platform")
    if not platform_name:
        raise ToolError("PLATFORM_UNSUPPORTED", "platform is required.")
    platform_name = str(platform_name)

    client_config = str(payload.get("config") or "Development")

    build_payload = payload.get("build") or {}
    if not isinstance(build_payload, dict):
        raise ToolError("BUILD_PATH_NOT_FOUND", "build must be an object containing 'path'.")
    build_path = _ensure_path(build_payload.get("path"), code="BUILD_PATH_NOT_FOUND", what="buildPath")

    timeout_seconds = _parse_timeout(payload)

    extra_args = _normalize_string_sequence(payload.get("extraArgs"), default=[])
    env_raw = payload.get("env") or {}
    if not isinstance(env_raw, dict):
        raise ToolError("INVALID_ENV", "env must be an object of key/value pairs.")
    env = {str(key): str(value) for key, value in env_raw.items()}

    return GauntletConfig(
        engine_root=engine_root,
        runuat_path=runuat_path,
        uproject=uproject,
        test_name=test_name,
        platform_name=str(platform_name),
        client_config=client_config,
        build_path=build_path,
        timeout_seconds=timeout_seconds,
        extra_args=extra_args,
        env=env,
    )


def _build_command(config: GauntletConfig) -> List[str]:
    command: List[str] = [
        str(config.runuat_path),
        "Gauntlet",
        f'-project="{config.uproject}"',
        f"-test={config.test_name}",
        f"-platform={config.platform_name}",
        f"-clientconfig={config.client_config}",
        f'-builds="{config.build_path}"',
        "-log",
        "-unattended",
    ]
    command.extend(config.extra_args)
    return command


def run_gauntlet(payload: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Execute a Gauntlet test run via RunUAT."""

    try:
        config = _parse_payload(payload)
    except ToolError as exc:
        return exc.to_response()

    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    log_path = make_log_path("gauntlet_uat", root=LOG_ROOT, timestamp=timestamp)

    command = _build_command(config)
    result: ProcessResult = spawn_process(
        command,
        log_path=log_path,
        env=config.env,
        timeout_seconds=config.timeout_seconds,
    )

    if result.timed_out:
        return ToolError(
            "TIMEOUT",
            "RunUAT Gauntlet timed out.",
            {"timeoutSeconds": config.timeout_seconds, "log": str(result.log_path)},
        ).to_response()

    if result.exit_code is None:
        return ToolError(
            "PROCESS_FAILED",
            "RunUAT did not provide an exit code.",
            {"log": str(result.log_path)},
        ).to_response()

    results, parse_error, gauntlet_log = _parse_gauntlet_log(result.log_path)

    response: Dict[str, Any] = {
        "ok": result.exit_code == 0,
        "exitCode": result.exit_code,
        "durationSec": int(result.duration_seconds),
        "uatCmd": format_command_for_display(command),
        "results": results,
        "logs": {"uatLog": str(result.log_path)},
    }

    if gauntlet_log:
        response["logs"]["gauntletLog"] = gauntlet_log

    if parse_error:
        response.setdefault(
            "error",
            {
                "code": "REPORT_PARSE_FAILED",
                "message": "Unable to parse Gauntlet summary from RunUAT log.",
                "details": {"log": str(result.log_path)},
            },
        )

    if result.exit_code != 0:
        response.setdefault(
            "error",
            {
                "code": "PROCESS_FAILED",
                "message": "RunUAT exited with a non-zero status.",
                "details": {
                    "exitCode": result.exit_code,
                    "log": str(result.log_path),
                    "tail": result.last_lines,
                },
            },
        )

    return response


__all__ = ["run_gauntlet"]
