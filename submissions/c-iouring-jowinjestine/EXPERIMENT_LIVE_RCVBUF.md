# Experimental design: live receive-buffer governor (not benchmark-ready)

## Goal

Change SO_RCVBUF on **already accepted** TCP sockets in response to cgroup memory pressure, without changing the fixed SQLite schema, request semantics, or connection fairness. This is distinct from the existing listener-only governor, which affects future accepts.

## Blocking issue

The current io_uring multishot accept uses **direct descriptors** (fixed-file table slots), not normal process file descriptors. Ordinary `setsockopt(fd, SOL_SOCKET, SO_RCVBUF, ...)` cannot use those slots. Calling it on a slot number would act on an unrelated process fd or return EBADF. Do **not** implement that shortcut.

## Proposed implementation

1. Prototype a separate opt-in acceptance path that obtains a normal fd, applies `SO_RCVBUF`, and safely registers/moves it into the ring fixed-file table. Preserve the slot/generation scheme and ensure no descriptor leaks. Measure registration cost.
2. For resizing existing connections, establish a safe mechanism to obtain a temporary normal fd referring to the same socket (subject to kernel support and privilege), or retain an owned normal fd with an explicit memory/fd budget. Never assume io_uring fixed-file slots are POSIX fds.
3. Maintain a bounded round-robin scan of live connections. Apply at most N updates per sampling interval; skip closing connections. Track actual effective buffer sizes and errors.
4. Hysteresis: reduce limits after sustained cgroup PSI/memory pressure, restore after recovery. Avoid oscillation and global mass updates in a single event-loop turn.
5. Benchmark at 280K, 300K and 320K against #486. Record TCP memory, retransmits, p95/p99, CPU, fd count, and update overhead.

## Exit criteria

Only enable in production when it passes 42/42 tests, remains compatible with the challenge kernel, and demonstrably improves the 5-minute hold. This PR is a **draft design**, not an implemented or benchmarkable optimization.
