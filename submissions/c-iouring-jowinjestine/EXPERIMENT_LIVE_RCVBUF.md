# Live receive-buffer governor experiment

## Implementation

This branch implements live SO_RCVBUF adjustment for accepted sockets using IORING_OP_FIXED_FD_INSTALL. It installs a **temporary ordinary fd** for an existing io_uring fixed-file slot, calls setsockopt on that ordinary fd, and closes it on every successful install completion. It never treats a fixed-file index as a POSIX fd.

The pre-existing listener governor still controls the requested cap for newly accepted sockets. Each accepted connection records the listener cap inherited at accept time; only connections whose last applied cap differs from the current governor cap are scheduled for an update.

## Controls

- TCP_MEM_GOVERNOR=1: existing listener governor, required for live updates
- TCP_LIVE_RCVBUF=1: enable bounded updates of existing accepted sockets; set 0 to restore baseline behavior
- TCP_LIVE_RCVBUF_BUDGET=512: maximum temporary fd installs per 2-second sampling interval (clamped at 8192)

The live updater uses a round-robin scan across rings and slots, marks each in-flight connection, checks the generation on completion, and closes the temporary fd even if the connection has been recycled. If the kernel rejects the operation as unsupported, it disables further attempts.

## Validation status

Code committed; **not compiled or tested on the challenge box**. Run build.sh and test/test.sh (42/42) and compare the five-minute hold against #486. Record live update counters, fd usage, TCP memory, p95/p99, and CPU. This is a performance experiment, not a demonstrated improvement.
