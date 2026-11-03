# C# / io_uring direct sockets / Native AOT

- **Language/runtime:** C# on .NET SDK 10.0.300 (`net10.0`), published as Native AOT for Linux x86-64. No NuGet packages beyond the AOT compiler; restore is locked.
- **HTTP:** a small hand-written HTTP/1.1 keep-alive server over io_uring, through a native C shim (`uring_server.c`) using liburing 2.15. Requires Linux 6.1+ (Ubuntu 24.04 ships 6.8).
- **SQLite driver:** direct P/Invoke to SQLite 3.53.4, built from source by `build.sh`. SQLite, liburing and the .NET SDK downloads are all checksum-verified.
- **Nginx or direct:** **direct.** The server listens on `HOST:PORT` (`0.0.0.0:80` on the benchmark box) and needs `CAP_NET_BIND_SERVICE`.

## Running

Run `sudo bash install.sh` once, then `bash build.sh` as an ordinary user, then `bash start.sh`. The server runs in the foreground and is configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`.

## Optimizations and why

The box has one vCPU, so everything below either cuts CPU per request or avoids serializing on it.

- **One thread does everything.** A single poll thread owns the io_uring rings and one SQLite connection. It parses each request, runs reads to completion inline, and queues the response. With no worker threads there are no cross-thread handoffs, locks, wakeups or context switches. Because one connection does all reads and writes, SQLite's page cache is never invalidated by another connection's commit.
- **One read transaction stays open between write groups.** This connection is the only writer, so the open read transaction always sees the latest commit. It is ended before every write group. This removes the WAL read-lock acquire and release (two `fcntl` calls) that every auto-commit read would otherwise pay. Every request still runs its SQL against SQLite.
- **Group commit with a 20 ms window.** Writes are collected for up to 20 ms (or 256 writes), then run in one `BEGIN IMMEDIATE` transaction:
  - Each write has its own savepoint, so one failing request rolls back alone.
  - Responses are sent only after `COMMIT` succeeds. If `COMMIT` fails, every write in that group gets a 500 and nothing from it is saved.
  - Larger groups mean fewer commits and fewer WAL page writes. The cost is up to 20 ms extra latency on writes only.
  - Savepoints and `BEGIN`/`COMMIT` are prepared once, not parsed per request.
- **WAL with `synchronous=NORMAL`.** Durability follows rule 6. Checkpoints run on a separate connection and thread (PASSIVE, with an occasional RESTART), so they never block request handling.
- **io_uring direct sockets.** Accept, receive and send all go through io_uring:
  - Descriptors are direct (registered) in 8 rings of 60,000 slots. A connection needs no ordinary file descriptor, so the server is not limited to 65,535 connections, and each ring's slot table fits under the 65,535 limit.
  - Receives are multishot from provided buffer rings, and sends and re-arms are submitted in batches per poll pass.
- **`SINGLE_ISSUER | DEFER_TASKRUN` rings.** Completion work runs in batches when the poll thread enters the kernel, instead of interrupting it. A wake eventfd is registered with every ring so that a sleeping `poll()` still wakes.
- **16,384 receive buffers per ring (256 MiB in total, paged in only as used).** Under load, a deferred batch uses a buffer for every ready socket at once. Smaller pools ran dry, ending receives with `ENOBUFS` and forcing them to be re-armed. That extra work deepened backlogs and caused multi-second latency spikes; the larger pools remove it.
- **Low-allocation hot path:**
  - SQL statements are prepared once and buffers are pooled.
  - JSON is written from byte templates in the required key order, with every value coming from SQLite.
  - JWTs are verified on spans. Every token's HMAC-SHA256 signature, `exp` and `nbf` are checked on every request.
- **Native AOT.** No JIT warm-up and a small runtime footprint.

## Prior C# entries

Several of these ideas appeared first in earlier C# submissions. Nothing was copied from them deliberately, but this entry was developed with AI coding assistants (Codex and Claude), so they may have drawn on these entries:

- `csharp-epoll-gichamba`: a hand-rolled Linux socket server in Native AOT that skips ASP.NET Core and talks to Linux and SQLite directly.
- `csharp-aspnetcore-vijay-highlo`: Native AOT with SQLite 3.53.4 built from source and hand-written `[LibraryImport]` bindings.
- `csharp-aspnetcore-maks-march`: one long-lived SQLite connection, requests run to completion on the I/O thread, and `Utf8JsonWriter` into pooled buffers.

This entry differs mainly in using io_uring direct sockets (beyond the 65,535 descriptor limit), a long-lived read transaction, and group commit.

## Rules notes

- No response, query-result or JWT caches. SQLite's own page cache is the only cache.
- The schema is untouched.
- Nothing on the box is tuned; all settings are inside the process.
