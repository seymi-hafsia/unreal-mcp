from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Any


@dataclass
class _DedupEntry:
    timestamp: float
    response: Dict[str, Any]


class DedupStore:
    """Simple in-memory deduplication store with a JSONL journal."""

    def __init__(
        self,
        ttl_sec: float = 600.0,
        journal_path: Optional[Path] = None,
        max_entries: int = 2048,
    ) -> None:
        self.ttl_sec = ttl_sec
        self.max_entries = max_entries
        self._lock = threading.Lock()
        self._entries: Dict[str, _DedupEntry] = {}
        self.journal_path = journal_path or Path("logs/dedup.jsonl")
        self._load_journal()

    def _load_journal(self) -> None:
        if not self.journal_path.exists():
            return

        now = time.time()
        try:
            with self.journal_path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        payload = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    request_id = payload.get("requestId")
                    response = payload.get("response")
                    ts = float(payload.get("ts", 0))

                    if not request_id or not isinstance(response, dict):
                        continue

                    if now - ts > self.ttl_sec:
                        continue

                    self._entries[request_id] = _DedupEntry(ts, response)
        except OSError:
            # If we fail to read the journal we simply start with an empty store.
            self._entries.clear()

    def _append_journal(self, request_id: str, response: Dict[str, Any]) -> None:
        try:
            self.journal_path.parent.mkdir(parents=True, exist_ok=True)
            with self.journal_path.open("a", encoding="utf-8") as handle:
                handle.write(
                    json.dumps(
                        {
                            "requestId": request_id,
                            "ts": time.time(),
                            "response": response,
                        },
                        ensure_ascii=False,
                    )
                    + "\n"
                )
        except OSError:
            # Ignore journaling errors; the in-memory map still guarantees correctness for
            # the current process lifetime.
            pass

    def get(self, request_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            self._gc_locked()
            entry = self._entries.get(request_id)
            if entry:
                return entry.response
            return None

    def put(self, request_id: str, response: Dict[str, Any]) -> None:
        now = time.time()
        with self._lock:
            self._gc_locked(now)
            self._entries[request_id] = _DedupEntry(now, response)
            if len(self._entries) > self.max_entries:
                # Remove oldest entries to bound memory usage.
                sorted_items = sorted(self._entries.items(), key=lambda item: item[1].timestamp)
                for key, _ in sorted_items[: len(self._entries) - self.max_entries]:
                    self._entries.pop(key, None)
            self._append_journal(request_id, response)

    def _gc_locked(self, now: Optional[float] = None) -> None:
        deadline = (now or time.time()) - self.ttl_sec
        stale = [key for key, entry in self._entries.items() if entry.timestamp < deadline]
        for key in stale:
            self._entries.pop(key, None)


__all__ = ["DedupStore"]

