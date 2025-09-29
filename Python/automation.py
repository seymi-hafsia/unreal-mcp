"""Shared helpers for automation-related MCP tools."""

from __future__ import annotations

import os
import shlex
import signal
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Mapping, MutableMapping, Optional, Sequence

__all__ = [
    "ToolError",
    "ProcessResult",
    "merge_env",
    "spawn_process",
    "format_command_for_display",
    "make_log_path",
]


class ToolError(Exception):
    """Custom exception used for structured tool errors."""

    def __init__(self, code: str, message: str, details: Optional[Dict[str, object]] = None):
        super().__init__(message)
        self.code = code
        self.details = details or {}

    def to_response(self) -> Dict[str, object]:
        return {
            "ok": False,
            "error": {
                "code": self.code,
                "message": str(self),
                "details": self.details,
            },
        }


@dataclass
class ProcessResult:
    """Represents the outcome of a spawned process."""

    exit_code: Optional[int]
    duration_seconds: float
    timed_out: bool
    log_path: Path
    last_lines: List[str]


def merge_env(custom: Optional[Mapping[str, object]]) -> Dict[str, str]:
    """Merge a custom environment mapping with the current environment."""

    env: MutableMapping[str, str] = dict(os.environ)
    if custom:
        for key, value in custom.items():
            env[str(key)] = str(value)
    return dict(env)


def _terminate_process(process: subprocess.Popen[str]) -> None:
    """Attempt to terminate the process tree for the given process."""

    if process.poll() is not None:
        return

    try:
        if os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)  # type: ignore[arg-type]
        else:
            os.killpg(process.pid, signal.SIGTERM)
    except Exception:
        try:
            process.terminate()
        except Exception:
            pass


def _kill_process(process: subprocess.Popen[str]) -> None:
    """Forcefully kill the given process."""

    if process.poll() is not None:
        return
    try:
        if os.name != "nt":
            os.killpg(process.pid, signal.SIGKILL)
        process.kill()
    except Exception:
        pass


def spawn_process(
    command: Sequence[object],
    *,
    log_path: Path,
    env: Optional[Mapping[str, object]] = None,
    timeout_seconds: Optional[int] = None,
) -> ProcessResult:
    """Spawn an external process, teeing stdout/stderr into a log file."""

    log_path = log_path.expanduser().resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)

    merged_env = merge_env(env)
    command_list = [str(part) for part in command]

    creationflags = 0
    preexec_fn = None
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        preexec_fn = os.setsid  # type: ignore[assignment]

    start_time = time.monotonic()
    process = subprocess.Popen(
        command_list,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=merged_env,
        creationflags=creationflags,
        preexec_fn=preexec_fn,
    )

    last_lines: deque[str] = deque(maxlen=50)

    def _reader() -> None:
        assert process.stdout is not None
        with open(log_path, "a", encoding="utf-8", errors="replace") as log_file:
            for line in iter(process.stdout.readline, ""):
                log_file.write(line)
                log_file.flush()
                last_lines.append(line.rstrip("\n"))

    with open(log_path, "w", encoding="utf-8", errors="replace"):
        pass

    reader_thread = threading.Thread(target=_reader, daemon=True)
    reader_thread.start()

    timed_out = False
    exit_code: Optional[int] = None

    try:
        exit_code = process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        _terminate_process(process)
        try:
            exit_code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            _kill_process(process)
            exit_code = process.wait()

    if process.stdout is not None:
        try:
            process.stdout.close()
        except Exception:
            pass

    reader_thread.join(timeout=5)

    duration_seconds = time.monotonic() - start_time

    return ProcessResult(
        exit_code=exit_code,
        duration_seconds=duration_seconds,
        timed_out=timed_out,
        log_path=log_path,
        last_lines=list(last_lines),
    )


def format_command_for_display(command: Sequence[object]) -> str:
    """Format a command list into a display-friendly string."""

    command_list = [str(part) for part in command]
    if os.name == "nt":
        return subprocess.list2cmdline(command_list)
    return shlex.join(command_list)


def make_log_path(
    prefix: str,
    *,
    root: Path,
    suffix: str = ".log",
    timestamp: Optional[str] = None,
) -> Path:
    """Return a timestamped log path under the given root."""

    if timestamp is None:
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    filename = f"{prefix}_{timestamp}{suffix}"
    return root.expanduser().resolve() / filename
