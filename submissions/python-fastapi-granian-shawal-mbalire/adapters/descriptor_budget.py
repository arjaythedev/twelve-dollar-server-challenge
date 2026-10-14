"""Keeps open file descriptors under the process limit by retiring keep-alive connections.

Every k6 user holds its own keep-alive connection, and the box allows 65,535 descriptors, so past
~65k users new connections would fail with EMFILE. While descriptor number `budget` is open, the
HTTP adapter answers with `Connection: close`, and clients reconnect for their next request. Idea
from PR #13; the near-limit budget follows PR #71.

POSIX hands out the lowest free descriptor, so descriptor `budget` is open only once every lower
one was taken: one fstat() is the whole probe. (Listing /proc/self/fd took 16 ms at 40k
descriptors and stalled the event loop.)
"""

import asyncio
import os
import resource
from collections.abc import Callable

RESERVE = 256  # descriptors left free for SQLite and new accepts


def descriptor_is_open(descriptor: int) -> bool:
    try:
        os.fstat(descriptor)
    except OSError:
        return False
    return True


class DescriptorBudget:
    """Resource. `strained` is True while descriptor `budget` is open."""

    __slots__ = ("strained", "_budget", "_interval_seconds", "_is_open", "_timer")

    def __init__(self, budget: int | None = None, interval_seconds: float = 0.25,
                 is_open: Callable[[int], bool] = descriptor_is_open):
        soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        limit = 65_535 if soft_limit == resource.RLIM_INFINITY else soft_limit
        ceiling = max(limit - RESERVE, limit // 2)
        self.strained = False
        self._budget = ceiling if budget is None else min(budget, ceiling)
        self._interval_seconds = interval_seconds
        self._is_open = is_open
        self._timer: asyncio.TimerHandle | None = None

    async def start(self) -> None:
        self._sample()

    async def close(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    def _sample(self) -> None:
        self.strained = self._is_open(self._budget)
        self._timer = asyncio.get_running_loop().call_later(self._interval_seconds, self._sample)
