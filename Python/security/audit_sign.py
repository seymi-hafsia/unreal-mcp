"""Audit log signing helpers."""

from __future__ import annotations

import base64
import hmac
import os
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from hashlib import sha256
from threading import RLock
from typing import Dict, Optional


def _canonicalize_payload(payload: Dict[str, object]) -> bytes:
    import json

    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


@dataclass
class AuditRecord:
    request_id: str
    tool: str
    payload: Dict[str, object]


class AuditSigner:
    def __init__(self, secret: Optional[str]) -> None:
        self.secret = secret.encode("utf-8") if secret else None
        self._lock = RLock()
        self._recent: Dict[str, float] = {}
        self.ttl = 3600.0

    def is_available(self) -> bool:
        return self.secret is not None

    def sign(self, record: AuditRecord) -> Optional[Dict[str, str]]:
        if not self.secret:
            return None
        payload = {"requestId": record.request_id, "tool": record.tool, **record.payload}
        server_ts = datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")
        payload_with_ts = {**payload, "serverTs": server_ts}
        canonical = _canonicalize_payload(payload_with_ts)
        signature = base64.b64encode(hmac.new(self.secret, canonical, sha256).digest()).decode("ascii")
        nonce = str(uuid.uuid4())
        timestamp = time.time()
        with self._lock:
            self._prune_locked(timestamp)
            self._recent[nonce] = timestamp
        return {"auditSig": signature, "nonce": nonce, "serverTs": server_ts}

    def verify_nonce(self, nonce: str) -> bool:
        with self._lock:
            timestamp = time.time()
            self._prune_locked(timestamp)
            if nonce in self._recent:
                return False
            self._recent[nonce] = timestamp
        return True

    def _prune_locked(self, now: float) -> None:
        expired = [key for key, ts in self._recent.items() if now - ts > self.ttl]
        for key in expired:
            self._recent.pop(key, None)


def load_secret_from_env(env_var: str) -> Optional[str]:
    value = os.getenv(env_var)
    if value:
        return value.strip()
    return None


__all__ = ["AuditRecord", "AuditSigner", "load_secret_from_env"]
