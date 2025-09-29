"""Implementation of the `automation.run_specs` MCP tool."""

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

LOG_ROOT = Path("logs") / "automation"
REPORT_ROOT = LOG_ROOT / "reports"

SUMMARY_RE = re.compile(
    r"LogAutomationController:\s+\w+:\s+Tests? Completed\. \(Pass/Fail/Skipped = (\d+)/(\d+)/(\d+)\)",
    re.IGNORECASE,
)
FAILURE_RE = re.compile(r"LogAutomationController:\s+(Error|Warning):\s+(.*)")


@dataclass
class AutomationSpecsConfig:
    """Parsed configuration for Automation RunTests invocations."""

    engine_root: Path
    uproject: Path
    tests: List[str]
    map_name: Optional[str]
    timeout_seconds: Optional[int]
    headless: bool
    extra_args: List[str]
    env: Dict[str, str]

    @property
    def editor_cmd(self) -> Path:
        binaries_dir = self.engine_root / "Engine" / "Binaries"
        system_name = platform.system().lower()
        if system_name.startswith("windows"):
            return binaries_dir / "Win64" / "UnrealEditor-Cmd.exe"
        if system_name.startswith("darwin") or "mac" in system_name:
            return binaries_dir / "Mac" / "UnrealEditor-Cmd"
        return binaries_dir / "Linux" / "UnrealEditor-Cmd"


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


def _build_exec_cmds(tests: Sequence[str]) -> str:
    commands = [f"Automation RunTests {test}" for test in tests if test]
    commands.append("Quit")
    return "; ".join(commands)


def _pick_report_xml(report_dir: Path) -> Optional[Path]:
    if not report_dir.exists():
        return None
    xml_files = sorted(report_dir.glob("*.xml"))
    if xml_files:
        return xml_files[0]
    return None


def _parse_failures(log_path: Path) -> List[Dict[str, str]]:
    failures: List[Dict[str, str]] = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            match = FAILURE_RE.search(line)
            if not match:
                continue
            remainder = match.group(2).strip()
            test_name = ""
            message = remainder
            if ":" in remainder:
                potential_test, potential_message = remainder.split(":", 1)
                if potential_test.strip():
                    test_name = potential_test.strip()
                    message = potential_message.strip()
            elif " - " in remainder:
                potential_test, potential_message = remainder.split(" - ", 1)
                test_name = potential_test.strip()
                message = potential_message.strip()
            failures.append({"test": test_name, "message": message})
    return failures


def _parse_summary(log_path: Path) -> Optional[Dict[str, int]]:
    with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
        for line in log_file:
            match = SUMMARY_RE.search(line)
            if match:
                passed = int(match.group(1))
                failed = int(match.group(2))
                skipped = int(match.group(3))
                total = passed + failed + skipped
                return {
                    "total": total,
                    "passed": passed,
                    "failed": failed,
                    "skipped": skipped,
                }
    return None


def _parse_results(log_path: Path) -> Tuple[Dict[str, Any], bool]:
    summary = _parse_summary(log_path)
    failures = _parse_failures(log_path)
    parse_error = summary is None
    if summary is None:
        summary = {"total": 0, "passed": 0, "failed": 0, "skipped": 0}
    summary["failures"] = failures
    return summary, parse_error


def _build_command(config: AutomationSpecsConfig, report_dir: Path) -> List[str]:
    command: List[str] = [str(config.editor_cmd), str(config.uproject)]
    command.append("-RunAutomationTests")
    command.append(f"-Project={config.uproject}")
    command.append(f'-ExecCmds="{_build_exec_cmds(config.tests)}"')
    command.extend(["-log", "-unattended", "-NoSplash"])
    if config.headless:
        command.append("-NullRHI")
    if config.map_name:
        command.append(f"-Map={config.map_name}")
    command.append(f"-ReportExportPath={report_dir}")
    command.extend(config.extra_args)
    return command


def _parse_payload(payload: Optional[Dict[str, Any]]) -> AutomationSpecsConfig:
    payload = payload or {}
    if not isinstance(payload, dict):
        raise ToolError("INVALID_PARAMS", "Expected params to be an object.")

    engine_root = _ensure_path(
        payload.get("engineRoot") or os.getenv("UE_ENGINE_ROOT"),
        code="ENGINE_NOT_FOUND",
        what="engineRoot",
    )
    editor_binary = engine_root / "Engine"
    if not editor_binary.exists():
        raise ToolError("ENGINE_NOT_FOUND", "engineRoot does not appear to be a valid Unreal Engine installation.")

    uproject = _ensure_path(payload.get("uproject"), code="UPROJECT_NOT_FOUND", what="uproject")

    tests = _normalize_string_sequence(payload.get("tests"), default=["All"])
    if not tests:
        tests = ["All"]

    timeout_seconds = _parse_timeout(payload)
    headless = bool(payload.get("headless", False))
    extra_args = _normalize_string_sequence(payload.get("extraArgs"), default=[])
    env_raw = payload.get("env") or {}
    if not isinstance(env_raw, dict):
        raise ToolError("INVALID_ENV", "env must be an object of key/value pairs.")
    env = {str(key): str(value) for key, value in env_raw.items()}

    map_name = payload.get("map")
    if map_name is not None:
        map_name = str(map_name)

    config = AutomationSpecsConfig(
        engine_root=engine_root,
        uproject=uproject,
        tests=tests,
        map_name=map_name,
        timeout_seconds=timeout_seconds,
        headless=headless,
        extra_args=extra_args,
        env=env,
    )

    editor_cmd = config.editor_cmd
    if not editor_cmd.exists():
        raise ToolError(
            "ENGINE_NOT_FOUND",
            "UnrealEditor-Cmd executable not found under engineRoot.",
            {"expected": str(editor_cmd)},
        )

    return config


def run_specs(payload: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Execute Unreal Automation Tests via UnrealEditor-Cmd."""

    try:
        config = _parse_payload(payload)
    except ToolError as exc:
        return exc.to_response()

    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    log_path = make_log_path("automation", root=LOG_ROOT, timestamp=timestamp)
    report_dir = (REPORT_ROOT / f"report_{timestamp}").expanduser().resolve()
    report_dir.mkdir(parents=True, exist_ok=True)

    command = _build_command(config, report_dir)

    result: ProcessResult = spawn_process(
        command,
        log_path=log_path,
        env=config.env,
        timeout_seconds=config.timeout_seconds,
    )

    if result.timed_out:
        return ToolError(
            "TIMEOUT",
            "UnrealEditor-Cmd timed out while running automation tests.",
            {"timeoutSeconds": config.timeout_seconds, "log": str(result.log_path)},
        ).to_response()

    if result.exit_code is None:
        return ToolError(
            "PROCESS_FAILED",
            "UnrealEditor-Cmd did not provide an exit code.",
            {"log": str(result.log_path)},
        ).to_response()

    summary, parse_error = _parse_results(result.log_path)
    report_xml = _pick_report_xml(report_dir)

    response: Dict[str, Any] = {
        "ok": result.exit_code == 0,
        "exitCode": result.exit_code,
        "durationSec": int(result.duration_seconds),
        "editorCmd": format_command_for_display(command),
        "results": summary,
        "logs": {"editorLog": str(result.log_path)},
    }

    if report_xml is not None:
        response["logs"]["reportXML"] = str(report_xml)

    if parse_error:
        response.setdefault("error", {
            "code": "REPORT_PARSE_FAILED",
            "message": "Unable to parse automation test summary from log.",
            "details": {"log": str(result.log_path)},
        })
    elif summary.get("failed"):
        failures = summary.get("failures", [])
        if failures:
            response.setdefault("error", {
                "code": "TESTS_FAILED",
                "message": "One or more automation tests failed.",
                "details": {"failures": failures[:10]},
            })

    if result.exit_code != 0:
        response.setdefault(
            "error",
            {
                "code": "PROCESS_FAILED",
                "message": "UnrealEditor-Cmd exited with a non-zero status.",
                "details": {
                    "exitCode": result.exit_code,
                    "log": str(result.log_path),
                    "tail": result.last_lines,
                },
            },
        )

    return response


__all__ = ["run_specs"]
