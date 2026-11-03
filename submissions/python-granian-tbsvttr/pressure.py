"""Linux descriptor pressure sampled at most once per 100 ms, after request work."""
import os
import errno
import resource
import time

limit = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
BUDGET = 40_000 if limit == resource.RLIM_INFINITY else max(1, min(40_000, limit - 128))
next_sample = 0.0
retiring = False


def connection_pressure():
    global next_sample, retiring
    now = time.monotonic()
    if now >= next_sample:
        next_sample = now + .1
        try:
            with os.scandir("/proc/self/fd") as descriptors:
                retiring = sum(1 for _ in descriptors) >= BUDGET
        except OSError as error:
            if error.errno in (errno.EMFILE, errno.ENFILE):
                retiring = True
            # Preserve the last decision otherwise; /proc is absent on macOS.
    return retiring
