# Cider io_uring v6

v6 is the poll-based Cider submission rebuilt with the public Cider v4 compiler. It keeps idle keep-alive connections on
an `io_uring` multishot poll and only submits a receive when the socket becomes
readable. The server source is the merged v5 source; v6 changes only the pinned
compiler and build configuration.

## Architecture

- 8 `SO_REUSEPORT` listeners and one event loop.
- Lazy rings: the 350k target uses six rings. Each ring has 65,535 fixed-file
  slots and a 65,536-entry completion queue.
- Idle connections hold a 48-byte `Conn` and a multishot
  `IORING_OP_POLL_ADD` (`POLLIN | POLLRDHUP`). They hold no input buffer.
- On `POLLIN`, v6 submits one `IORING_OP_RECV` with a provided buffer
  (1024 × 2048 bytes per ring). The read is one-shot; the poll stays armed.
  A full read is drained with another one-shot receive.
- Requests are parsed one per connection per batch. Reads run from one SQLite
  snapshot, writes run after, and the batch commits once.
- Responses are built in shared per-batch arenas and sent directly from them.
  There is no per-connection output buffer.
- Idle keep-alive closes after 90 seconds. Close cancels both the poll and any
  in-flight receive before closing the fixed descriptor.

## Validation

Local cloud server: kernel `6.8.0-117-generic`, 1 vCPU, 2 GB, six source IPs,
pinned Cider v4 compiler `e54015ec83aa2b5fb814ed0e8ecac0c942688fb5`.

Full official-rate load command (`THINK_SCALE=1.0`, 60s ramp + 300s hold):

```sh
leanload-400k 104.204.68.149 80 270000 60 300 4 /opt/loadtest/tokens.json
```

| Build | TOTAL requests | TOTAL HTTP failures | TOTAL p95 / p99 | HOLD requests | HOLD HTTP failures | HOLD p95 / p99 | HOLD req/s | CPU / request |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| v4 `--codegen=fast` | 9,461,320 | 998 (0.011%) | 26 ms / 98 ms | 8,462,114 | 951 (0.011%) | 27 ms / 94 ms | 28,207 | 27.96 µs |

The v6 build uses the v4 `--codegen=fast` backend. It completed the full 270k
official-rate hold without an OOM or stall, with 998 total HTTP failures
(0.011%) and 951 HOLD failures (0.011%).

The load generator reported high `connfail` counts on the 270k run; v5 experience
attributes this to client reconnect/source-port behavior, not server accept-queue
overflow. The local and server conformance suites both passed 42/42 before the
benchmark. No official score is claimed for v6.

## Build and run

v6 pins `Pomilon/Cider` at `e54015ec83aa2b5fb814ed0e8ecac0c942688fb5` and
installs it under `/opt/cider-v4`, leaving the v5 `/opt/cider` install intact.

```sh
CIDER_BIN=/opt/cider-v4/build/cider CIDER_LIB=/opt/cider-v4/modules bash build.sh
PORT=80 SQLITE_PATH=/opt/challenge/run/feed.db \
  JWT_SECRET=twelve-dollar-challenge ./bin/server
```

`install.sh` builds the static Cider toolchain and installs the server. The
merged v5 submission is untouched; v6 is source-only and adds no build output.
