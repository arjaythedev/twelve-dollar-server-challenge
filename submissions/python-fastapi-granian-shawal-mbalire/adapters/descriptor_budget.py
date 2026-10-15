"""Keeps open file descriptors under the process limit by retiring keep-alive connections.

Every k6 user holds its own keep-alive connection, and the box allows 65,535 descriptors, so past
~65k users new connections would fail with EMFILE. While `budget` descriptors are in use, the HTTP
adapter answers with `Connection: close`, and clients reconnect for their next request. Idea from
PR #13; the near-limit budget follows PR #71.

How many are in use: on Linux, the kernel's count of open TCP sockets in this network namespace
(/proc/self/net/sockstat, "TCP: inuse"), one small read. On the box nearly all of them are ours.
Elsewhere (a Mac), whether descriptor `budget` is open: POSIX hands out the lowest free descriptor,
so it opens only once every lower one was taken. That probe is wrong once connections close below
it: at 130k users the descriptor stayed open while its idle owner thought for up to 15 s, every
response meanwhile said `Connection: close`, and open sockets drained from 62k to 8k in waves.
(Listing /proc/self/fd took 16 ms at 40k descriptors and stalled the event loop.)
"""

import asyncio
import os
import resource
from collections.abc import Callable

# Descriptors left free for SQLite and for the connections accepted between two samples: past 65k
# users 7,000-8,000 connections a second arrive, so 256 ran out between 0.25 s samples, accepts failed
# and the kernel's accept queue overflowed (clients then wait 1 s+ to retry).
RESERVE = 4096
SOCKSTAT = "/proc/self/net/sockstat"


def descriptor_is_open(descriptor: int) -> bool:
    try:
        os.fstat(descriptor)
    except OSError:
        return False
    return True


def tcp_sockets_in_use(sockstat: bytes) -> int:
    """The "TCP: inuse N" count from /proc/net/sockstat's text."""
    start = sockstat.index(b"TCP: inuse ") + 11
    return int(sockstat[start:sockstat.index(b" ", start)])


class SocketCounter:
    """`at_budget` from sockstat: the file stays open and is re-read with one pread per sample."""

    __slots__ = ("_descriptor",)

    def __init__(self, path: str = SOCKSTAT):
        self._descriptor = os.open(path, os.O_RDONLY)

    def __call__(self, budget: int) -> bool:
        return tcp_sockets_in_use(os.pread(self._descriptor, 4096, 0)) >= budget


def default_at_budget() -> Callable[[int], bool]:
    try:
        return SocketCounter()
    except OSError:
        return descriptor_is_open


class DescriptorBudget:
    """Resource. `strained` is True while `budget` descriptors are in use, as `at_budget` judges it."""

    __slots__ = ("strained", "_budget", "_interval_seconds", "_at_budget", "_timer")

    def __init__(self, budget: int | None = None, interval_seconds: float = 0.05,
                 at_budget: Callable[[int], bool] | None = None):
        soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
        limit = 65_535 if soft_limit == resource.RLIM_INFINITY else soft_limit
        ceiling = max(limit - RESERVE, limit // 2)
        self.strained = False
        self._budget = ceiling if budget is None else min(budget, ceiling)
        self._interval_seconds = interval_seconds
        self._at_budget = at_budget if at_budget is not None else default_at_budget()
        self._timer: asyncio.TimerHandle | None = None

    async def start(self) -> None:
        self._sample()

    async def close(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    def _sample(self) -> None:
        self.strained = self._at_budget(self._budget)
        self._timer = asyncio.get_running_loop().call_later(self._interval_seconds, self._sample)
