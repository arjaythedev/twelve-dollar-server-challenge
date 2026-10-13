"""Keeps open file descriptors under the process limit by retiring keep-alive connections.

Every k6 user holds its own keep-alive connection, and the box allows 65,535 descriptors, so past
~65k users new connections would fail with EMFILE. Once a second this samples how many descriptors
are open (Linux /proc only). While the count is over budget the HTTP adapter answers with
`Connection: close`, and clients reconnect for their next request. Idea from PR #13.
"""

import asyncio
import os
import resource

PROC_FDS = "/proc/self/fd"
RESERVE = 1024  # descriptors left free under a small limit, for SQLite and new accepts


class FdBudget:
    """Resource. `strained` is True while open descriptors exceed the budget."""

    __slots__ = ("strained", "_budget", "_every_s", "_fd_dir", "_timer")

    def __init__(self, budget: int = 40_000, every_s: float = 1.0, fd_dir: str = PROC_FDS):
        soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft != resource.RLIM_INFINITY:
            budget = min(budget, max(soft - RESERVE, soft // 2))
        self.strained = False
        self._budget = budget
        self._every_s = every_s
        self._fd_dir = fd_dir
        self._timer: asyncio.TimerHandle | None = None

    async def start(self) -> None:
        if os.path.isdir(self._fd_dir):
            self._sample()

    async def close(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    def _sample(self) -> None:
        try:
            self.strained = len(os.listdir(self._fd_dir)) > self._budget
        except OSError:  # EMFILE: listing the directory needs a descriptor itself
            self.strained = True
        self._timer = asyncio.get_running_loop().call_later(self._every_s, self._sample)
