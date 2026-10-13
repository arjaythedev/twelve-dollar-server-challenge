"""Clock port backed by the system clocks."""

import time


class SystemClock:
    __slots__ = ()

    wall = staticmethod(time.time)
    monotonic = staticmethod(time.monotonic)
