"""Token bucket rate limiting helpers."""

from __future__ import annotations

import time
from collections import defaultdict, deque
from dataclasses import dataclass
from typing import Deque, Dict, Tuple


@dataclass
class RateLimitConfig:
    per_minute_global: int
    per_minute_tool: int


class RateLimiter:
    def __init__(self, config: RateLimitConfig) -> None:
        self.config = config
        self.window = 60.0
        self.global_hits: Deque[float] = deque()
        self.tool_hits: Dict[str, Deque[float]] = defaultdict(deque)

    def _prune(self, hits: Deque[float], now: float) -> None:
        while hits and now - hits[0] > self.window:
            hits.popleft()

    def check(self, tool: str) -> Tuple[bool, float]:
        now = time.time()
        self._prune(self.global_hits, now)
        self._prune(self.tool_hits[tool], now)

        if len(self.global_hits) >= self.config.per_minute_global:
            retry = max(0.0, self.window - (now - self.global_hits[0]))
            return False, retry

        if len(self.tool_hits[tool]) >= self.config.per_minute_tool:
            retry = max(0.0, self.window - (now - self.tool_hits[tool][0]))
            return False, retry

        self.global_hits.append(now)
        self.tool_hits[tool].append(now)
        return True, 0.0


__all__ = ["RateLimitConfig", "RateLimiter"]
