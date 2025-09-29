"""Utilities for invoking Unreal Automation Tool (RunUAT) BuildCookRun."""

from __future__ import annotations

import os
import queue
import signal
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

LOG_ROOT = Path("logs") / "uat"
DEFAULT_ARCHIVE_ROOT = Path("builds")
SUPPORTED_PLATFORMS = {
    "win64": "Win64",
    "windows": "Win64",
    "win": "Win64",
    "linux": "Linux",
    "linuxarm64": "LinuxArm64",
    "mac": "Mac",
    "macos": "Mac",
    "ios": "IOS",
    "android": "Android",
    "hololens": "HoloLens",
    "ps5": "PS5",
    "xboxseriesx": "XSX",
    "xsx": "XSX",
}
INTERESTING_ARTIFACT_PATTERNS = [
    "*.exe",
    "*.app",
    "*.apk",
    "*.ipa",
    "*.pak",
    "*.ucas",
    "*.utoc",
    "*.zip",
    "*.tar",
]


class ToolError(Exception):
    """Custom exception used for structured tool errors."""

    def __init__(self, code: str, message: str, details: Optional[Dict[str, Any]] = None):
        super().__init__(message)
        self.code = code
        self.details = details or {}

    def to_response(self) -> Dict[str, Any]:
        return {
            "ok": False,
            "error": {
                "code": self.code,
                "message": str(self),
                "details": self.details,
            },
        }


@dataclass
class BuildCookRunConfig:
    """Configuration for a BuildCookRun invocation."""

    engine_root: Path
    runuat_path: Path
    uproject: Path
    project_name: str
    target: str
    configuration: str
    cook: bool
    stage: bool
    package: bool
    archive: bool
    archive_dir: Optional[Path]
    pak: bool
    iostore: bool
    prereqs: bool
    build: bool
    nativize: bool
    nodebuginfo: bool
    compressed: bool
    maps: Optional[List[str]]
    extra_args: List[str]
    env: Dict[str, str]
    timeout_seconds: Optional[int]
    dry_run: bool
    platforms: List[str]
    timestamp: str = field(default_factory=lambda: datetime.now().strftime("%Y%m%d_%H%M%S"))

    @property
    def is_windows(self) -> bool:
        return os.name == "nt"

    @property
    def project_dir(self) -> Path:
        return self.uproject.parent

    @classmethod
    def from_payload(cls, payload: Optional[Dict[str, Any]]) -> "BuildCookRunConfig":
        payload = payload or {}

        engine_root_str = payload.get("engineRoot") or os.getenv("UE_ENGINE_ROOT")
        if not engine_root_str:
            raise ToolError("ENGINE_NOT_FOUND", "engineRoot is required (or UE_ENGINE_ROOT env).")
        engine_root = Path(engine_root_str).expanduser().resolve()
        if not engine_root.exists():
            raise ToolError("ENGINE_NOT_FOUND", "engineRoot does not exist.", {"engineRoot": str(engine_root)})

        runuat_name = "RunUAT.bat" if os.name == "nt" else "RunUAT.sh"
        runuat_path = engine_root / "Engine" / "Build" / "BatchFiles" / runuat_name
        if not runuat_path.exists():
            raise ToolError(
                "ENGINE_NOT_FOUND",
                "RunUAT script not found under engineRoot.",
                {"expected": str(runuat_path)},
            )

        uproject_str = payload.get("uproject")
        if not uproject_str:
            raise ToolError("UPROJECT_NOT_FOUND", "uproject path is required.")
        uproject = Path(uproject_str).expanduser().resolve()
        if not uproject.exists():
            raise ToolError("UPROJECT_NOT_FOUND", "uproject file does not exist.", {"uproject": str(uproject)})

        platforms_raw = payload.get("platforms") or []
        if isinstance(platforms_raw, str):
            platforms_iter: Sequence[str] = [platforms_raw]
        elif isinstance(platforms_raw, Sequence):
            platforms_iter = platforms_raw
        else:
            raise ToolError("PLATFORM_UNSUPPORTED", "platforms must be a string or list of strings.")

        platforms: List[str] = []
        for platform_value in platforms_iter:
            normalized = normalize_platform(platform_value)
            if not normalized:
                raise ToolError(
                    "PLATFORM_UNSUPPORTED",
                    f"Unsupported platform '{platform_value}'.",
                    {"platform": platform_value},
                )
            platforms.append(normalized)
        if not platforms:
            raise ToolError("PLATFORM_UNSUPPORTED", "At least one platform is required.")

        project_name = uproject.stem
        target = payload.get("target") or project_name
        configuration = payload.get("configuration") or "Development"

        archive = bool(payload.get("archive", False))
        archive_dir_str = payload.get("archiveDir")
        archive_dir = Path(archive_dir_str).expanduser() if archive_dir_str else None
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        if archive and archive_dir is None:
            archive_dir = DEFAULT_ARCHIVE_ROOT / f"{project_name}_{timestamp}"

        maps = payload.get("maps")
        if isinstance(maps, str):
            maps = [maps]
        elif maps is not None:
            maps = [str(m) for m in maps]

        extra_args_raw = payload.get("extraArgs") or []
        if isinstance(extra_args_raw, str):
            extra_args = [extra_args_raw]
        else:
            extra_args = [str(arg) for arg in extra_args_raw]

        env_raw = payload.get("env") or {}
        env = {str(key): str(value) for key, value in env_raw.items()}

        timeout_minutes = payload.get("timeoutMinutes")
        timeout_seconds = None
        if timeout_minutes is not None:
            try:
                timeout_value = float(timeout_minutes)
            except (TypeError, ValueError):
                raise ToolError("INVALID_TIMEOUT", "timeoutMinutes must be numeric.")
            if timeout_value > 0:
                timeout_seconds = int(timeout_value * 60)

        return cls(
            engine_root=engine_root,
            runuat_path=runuat_path,
            uproject=uproject,
            project_name=project_name,
            target=str(target),
            configuration=str(configuration),
            cook=bool(payload.get("cook", False)),
            stage=bool(payload.get("stage", False)),
            package=bool(payload.get("package", False)),
            archive=archive,
            archive_dir=archive_dir,
            pak=bool(payload.get("pak", False)),
            iostore=bool(payload.get("iostore", False)),
            prereqs=bool(payload.get("prereqs", False)),
            build=bool(payload.get("build", False)),
            nativize=bool(payload.get("nativize", False)),
            nodebuginfo=bool(payload.get("nodebuginfo", False)),
            compressed=bool(payload.get("compressed", False)),
            maps=maps,
            extra_args=extra_args,
            env=env,
            timeout_seconds=timeout_seconds,
            dry_run=bool(payload.get("dryRun", False)),
            platforms=platforms,
            timestamp=timestamp,
        )


def normalize_platform(value: Any) -> Optional[str]:
    if not isinstance(value, str):
        return None
    key = value.strip().lower()
    return SUPPORTED_PLATFORMS.get(key) or (value if value else None)


def run_buildcookrun(payload: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Entry point exposed as MCP tool for BuildCookRun."""

    try:
        config = BuildCookRunConfig.from_payload(payload)
    except ToolError as exc:  # Configuration error, return immediately
        return exc.to_response()

    if config.archive and config.archive_dir and not config.dry_run:
        config.archive_dir.mkdir(parents=True, exist_ok=True)

    results: List[Dict[str, Any]] = []
    for platform_name in config.platforms:
        if config.dry_run:
            result = build_dry_run_result(config, platform_name)
        else:
            result = execute_buildcookrun(config, platform_name)
        results.append(result)

    if len(results) == 1:
        return results[0]

    ok = all(result.get("ok", False) for result in results)
    return {
        "ok": ok,
        "results": results,
    }


def build_dry_run_result(config: BuildCookRunConfig, platform_name: str) -> Dict[str, Any]:
    command_parts = build_base_command(config, platform_name)
    command_line = format_command_line(config, command_parts)

    project_saved = config.project_dir / "Saved"
    would_write: List[str] = []
    if config.archive and config.archive_dir:
        would_write.append(str(config.archive_dir.resolve()))
    if config.cook:
        would_write.append(str((project_saved / "Cooked").resolve()))
    if config.stage:
        would_write.append(str((project_saved / "StagedBuilds").resolve()))
    if config.package:
        would_write.append(str((project_saved / "Logs").resolve()))

    return {
        "ok": True,
        "dryRun": True,
        "exitCode": None,
        "commandLine": command_line,
        "platform": platform_name,
        "durationSec": 0,
        "logs": {},
        "wouldWriteTo": sorted(dict.fromkeys(would_write)),
    }


def execute_buildcookrun(config: BuildCookRunConfig, platform_name: str) -> Dict[str, Any]:
    command_parts = build_base_command(config, platform_name)
    command_line = format_command_line(config, command_parts)

    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = LOG_ROOT / f"uat_{config.project_name}_{platform_name}_{timestamp}.log"

    env = os.environ.copy()
    env.update(config.env)
    if "UE_ENGINE_ROOT" not in env:
        env["UE_ENGINE_ROOT"] = str(config.engine_root)

    process_args = build_process_args(config, command_parts)

    creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if config.is_windows else 0
    preexec_fn = os.setsid if not config.is_windows else None

    start_time = time.monotonic()
    timed_out = False
    tail_lines: List[str] = []

    with log_path.open("w", encoding="utf-8", errors="ignore") as log_file:
        process = subprocess.Popen(
            process_args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=str(config.engine_root),
            env=env,
            bufsize=1,
            universal_newlines=True,
            creationflags=creationflags,
            preexec_fn=preexec_fn,
        )
        try:
            timed_out, tail_lines = stream_output(process, log_file, config.timeout_seconds)
        finally:
            if timed_out and process.poll() is None:
                terminate_process(process, config.is_windows)
            process.wait()

    duration = int(time.monotonic() - start_time)

    exit_code = process.returncode if process.returncode is not None else -1
    parsed_exit, highlights, last_lines = parse_uat_log(log_path)
    if parsed_exit is not None:
        exit_code = parsed_exit

    logs: Dict[str, Any] = {"uatLog": str(log_path.resolve())}
    cook_log = find_latest_cook_log(config.project_dir)
    if cook_log:
        logs["cookLog"] = cook_log

    artifacts: List[str] = []
    if config.archive and config.archive_dir:
        artifacts = collect_artifacts(config.archive_dir)

    result: Dict[str, Any] = {
        "ok": not timed_out and exit_code == 0,
        "exitCode": exit_code,
        "durationSec": duration,
        "commandLine": command_line,
        "platform": platform_name,
        "logs": logs,
        "dryRun": False,
    }

    if highlights:
        result["highlights"] = highlights
    if artifacts:
        result["artifacts"] = artifacts

    if timed_out:
        with log_path.open("a", encoding="utf-8", errors="ignore") as log_file:
            log_file.write(
                f"\n[MCP] BuildCookRun timed out after {config.timeout_seconds} seconds.\n"
            )
        result["ok"] = False
        result["error"] = {
            "code": "TIMEOUT",
            "message": "RunUAT timed out before completion.",
            "details": {
                "timeoutSeconds": config.timeout_seconds,
                "uatLog": str(log_path.resolve()),
                "lastLines": tail_lines[-20:],
            },
        }
        result["timedOut"] = True
        return result

    if exit_code != 0:
        result["ok"] = False
        result["error"] = {
            "code": "PROCESS_FAILED",
            "message": "RunUAT returned a non-zero exit code.",
            "details": {
                "exitCode": exit_code,
                "uatLog": str(log_path.resolve()),
                "lastLines": last_lines[-20:] or tail_lines[-20:],
            },
        }
        return result

    if config.archive and config.archive_dir and not artifacts:
        result.setdefault("warnings", []).append(
            {
                "code": "ARTIFACTS_NOT_FOUND",
                "message": "No build artifacts were discovered.",
                "details": {"archiveDir": str(config.archive_dir.resolve())},
            }
        )

    return result


def build_base_command(config: BuildCookRunConfig, platform_name: str) -> List[str]:
    command = [
        "BuildCookRun",
        f"-project={str(config.uproject)}",
        "-noP4",
        f"-platform={platform_name}",
        f"-clientconfig={config.configuration}",
    ]
    if config.target:
        command.append(f"-target={config.target}")
    if config.cook:
        command.append("-cook")
    if config.stage:
        command.append("-stage")
    if config.pak:
        command.append("-pak")
    if config.iostore:
        command.append("-iostore")
    if config.package:
        command.append("-package")
    if config.archive:
        command.append("-archive")
        if config.archive_dir:
            command.append(f"-archivedirectory={str(config.archive_dir)}")
    if config.build:
        command.append("-build")
    if config.prereqs:
        command.append("-prereqs")
    if config.compressed:
        command.append("-compressed")
    if config.nodebuginfo:
        command.append("-nodebuginfo")
    if config.nativize:
        command.append("-nativizeassets")

    if config.maps:
        lowered = [m.lower() for m in config.maps]
        if lowered and all(m == "all" for m in lowered):
            command.append("-allmaps")
        else:
            for map_name in config.maps:
                command.append(f"-map={map_name}")

    command.extend(config.extra_args)
    return command


def build_process_args(config: BuildCookRunConfig, command_parts: List[str]) -> List[str]:
    base = [str(config.runuat_path)] + command_parts
    if config.is_windows:
        return ["cmd.exe", "/c"] + base
    return base


def format_command_line(config: BuildCookRunConfig, command_parts: List[str]) -> str:
    base = [str(config.runuat_path)] + command_parts
    if config.is_windows:
        return subprocess.list2cmdline(base)
    return " ".join(subprocess.list2cmdline([part]) if " " in part else part for part in base)


def stream_output(
    process: subprocess.Popen[str],
    log_file,
    timeout_seconds: Optional[int],
) -> tuple[bool, List[str]]:
    tail = deque(maxlen=50)
    q: "queue.Queue[Optional[str]]" = queue.Queue()

    def reader() -> None:
        try:
            assert process.stdout is not None
            for line in process.stdout:
                q.put(line)
        finally:
            q.put(None)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    deadline = time.monotonic() + timeout_seconds if timeout_seconds else None
    timed_out = False

    while True:
        try:
            item = q.get(timeout=0.1)
        except queue.Empty:
            if deadline and time.monotonic() > deadline:
                timed_out = True
                break
            if process.poll() is not None and not thread.is_alive() and q.empty():
                break
            continue

        if item is None:
            break
        log_file.write(item)
        log_file.flush()
        tail.append(item.strip())

        if deadline and time.monotonic() > deadline:
            timed_out = True
            break

    thread.join(timeout=1.0)
    return timed_out, list(tail)


def terminate_process(process: subprocess.Popen[str], is_windows: bool) -> None:
    try:
        if is_windows:
            ctrl_break = getattr(signal, "CTRL_BREAK_EVENT", None)
            if ctrl_break is not None:
                process.send_signal(ctrl_break)
                time.sleep(0.5)
            process.kill()
        else:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                return
            for _ in range(10):
                if process.poll() is not None:
                    break
                time.sleep(0.2)
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def parse_uat_log(log_path: Path) -> tuple[Optional[int], List[str], List[str]]:
    exit_code: Optional[int] = None
    highlights: List[str] = []
    last_lines: deque[str] = deque(maxlen=50)

    if not log_path.exists():
        return exit_code, highlights, list(last_lines)

    with log_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            last_lines.append(line)
            if "AutomationTool exiting with ExitCode=" in line:
                try:
                    exit_code = int(line.split("ExitCode=")[-1].split()[0].strip(".[]"))
                except ValueError:
                    pass
            if "Cooked content" in line or "Cooked packages" in line:
                if line not in highlights:
                    highlights.append(line)
            if "Pak" in line and "completed" in line:
                if line not in highlights:
                    highlights.append(line)
            if "Archive" in line and "directory" in line:
                if line not in highlights:
                    highlights.append(line)
            if line.startswith("PackagingResults:"):
                if line not in highlights:
                    highlights.append(line)
            if "BUILD SUCCESS" in line or "BUILD SUCCESSFUL" in line:
                if line not in highlights:
                    highlights.append(line)
            if "BUILD FAILED" in line or "Packaging failed" in line:
                if line not in highlights:
                    highlights.append(line)
            if "PROJECT PACKAGED" in line:
                if line not in highlights:
                    highlights.append(line)
    return exit_code, highlights, list(last_lines)


def find_latest_cook_log(project_dir: Path) -> Optional[str]:
    logs_dir = project_dir / "Saved" / "Logs"
    if not logs_dir.exists():
        return None
    candidates = list(logs_dir.glob("Cook-*.log"))
    if not candidates:
        return None
    latest = max(candidates, key=lambda item: item.stat().st_mtime)
    return str(latest.resolve())


def collect_artifacts(archive_dir: Path) -> List[str]:
    if not archive_dir.exists():
        return []

    artifacts: List[str] = []
    for pattern in INTERESTING_ARTIFACT_PATTERNS:
        for path in archive_dir.rglob(pattern):
            artifacts.append(str(path.resolve()))

    if not artifacts:
        for path in sorted(archive_dir.glob("**/*")):
            if path.is_file():
                artifacts.append(str(path.resolve()))
            elif path.is_dir():
                artifacts.append(str(path.resolve()))
            if len(artifacts) >= 20:
                break

    unique: List[str] = []
    seen = set()
    for item in artifacts:
        if item not in seen:
            seen.add(item)
            unique.append(item)
    return unique

