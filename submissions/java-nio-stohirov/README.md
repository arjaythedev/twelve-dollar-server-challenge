# Java + io_uring

| | |
|---|---|
| Language | Java 25: Eclipse Temurin `jdk-25.0.4.1+1`, official tarball, sha256-pinned in `install.sh` |
| Framework | None. A hand-written HTTP/1.1 server on Linux `io_uring`, driven from Java through FFM and shared memory |
| SQLite driver | No JDBC. FFM (`java.lang.foreign`) bindings to SQLite **3.54.0**, compiled from the amalgamation by `build.sh` (sha256-pinned) |
| Reads | SQLite's own B-tree cursors, through `csrc/shim.c`, the one C file |
| Dependencies | Zero Java dependencies. `build.sh` needs only `cc` and the JDK |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl; Temurin 25 to /opt/jdk
bash build.sh          # SQLite + csrc/shim.c -> build/libshim.so, sources -> build/server.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

It needs Linux 6.1 or newer with io_uring allowed, and has no fallback. Docker's default seccomp profile can
block io_uring, so a container may need `--security-opt seccomp=unconfined`.

## Where the code is

Java does everything it can reach: HTTP, the io_uring rings, JWT, routing, writes and checkpoints. The C shim exists
because SQLite's B-tree cursors are private to the amalgamation, so nothing outside it can call them.

| | |
|---|---|
| `Server.java` | the event loop, connections, HTTP parsing and handlers |
| `Ring.java` | one io_uring: its queues, direct descriptors and receive buffers |
| `Db.java`, `Sqlite.java` | the pass's transaction, writes, SQL fallbacks, FFM bindings |
| `Checkpointer.java` | WAL checkpoints on their own thread |
| `Sys.java` | system calls through FFM |
| `Jwt.java`, `Json.java`, `Out.java` | HS256, request JSON, the response arena |
| `csrc/shim.c` | B-tree reads and their JSON, the transaction around them |

The read plan, the transaction per pass and the checkpoint hold follow
[`rust-iouring-kensac`](../rust-iouring-kensac), which worked them out first and documents them well.

## Optimizations, and why

**Connections past the descriptor limit**
- **Direct descriptors.** Accepted sockets live in a ring's file table, not the process's, so 300,000 connections
  hold about 25 file descriptors under `LimitNOFILE=65535`. The server no longer closes idle connections, which was
  the old design's limit: it held 102,000 users at 66% CPU with 0.85% errors, all of them reconnects.
- **Up to 16 rings, added as needed.** A ring's table can be no larger than `RLIMIT_NOFILE`, so each ring holds 65,535.
  It starts with one. Accepting moves to the next ring within a margin of full, and the accepts in flight always
  fit inside that margin, so the kernel is never asked for a slot it doesn't have.
- **Eight `SO_REUSEPORT` listeners.** A listen queue is capped at `somaxconn`, so eight of them absorb a ramp or a
  slow pass. Up to 512 accepts stay in flight.
- **No buffers while idle.** Each connection has one multishot receive on a ring of 1,024 provided 2 KB buffers, and
  borrows one only while bytes are arriving. A connection is one small Java object.

**The event loop**
- **One thread drives every ring and runs SQLite inline.** A pass submits what the last pass queued, collects
  completions, answers requests, commits, and queues the sends. That is one system call per ring per pass, however
  many requests it holds. There is no `read`, `write`, `accept` or `epoll` system call per request.
- **Rings are read as shared memory.** Queue entries are read and written from Java with `MemorySegment`, and the JIT
  compiles that to plain loads and stores with the barriers the protocol needs. Only `io_uring_enter` is a downcall.
- **`DEFER_TASKRUN` and a batched wait.** Completions run when the loop asks, not by interrupting it. Ring 0 waits for
  256 completions or 1 ms, so bigger passes share one commit.
- **Responses come from two off-heap arenas.** Sends use `MSG_DONTWAIT`, so the kernel finishes them while reading the
  queue and never keeps the address. A short send's remainder is only learned in the next pass, so each pass
  writes to the arena the last one didn't. A connection has one send in flight, and what it can't take waits on a
  `POLLOUT` in its own heap buffer.
- **`SHUTDOWN` hard-linked to a fixed-file `CLOSE`.** The shutdown ends the multishot receive, which would otherwise
  keep the socket alive. A generation number in `user_data` drops completions meant for a reused slot.

**SQLite**
- **One transaction per pass.** Reads share a snapshot and writes are group-committed. **No response from a pass is
  sent until its `COMMIT` returns** (rule 6). If a commit fails, the pass's connections are closed.
- **Reads go through B-tree cursors, not the SQL engine** (rule 5: every request reads live rows). The feed walks
  `posts_created_at_id_idx`, looks the posts up in ascending id order so consecutive ids are a step along one leaf,
  finds the authors, and counts likes in one walk of `likes_post_id_idx`. Data the plan doesn't expect, like a post
  whose user has no row, is answered by the plain SQL query instead. At startup the server checks the column order of
  everything it decodes and refuses to run on a different schema.
- **Checkpoints on their own thread at nice 19**, on a second connection, so the loop never waits for their fsyncs.
  From 4,096 WAL frames it runs a PASSIVE checkpoint, then holds writes (reads go on, unread writes wait in their
  connection's buffer) while it copies the last frames, for at most 250 ms. The next write restarts the log. Past
  65,536 frames the loop checkpoints by itself, as a last resort. This needs `SQLITE_THREADSAFE=2`.
- **`unix-excl` VFS.** The process keeps an exclusive lock on the file and its two connections share the WAL index in
  memory: no `-shm` file and no file lock per transaction.
- **Pragmas:** `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `cache_size=-65536`, `temp_store=MEMORY`,
  `mmap_size=0` (a cached page keeps its parsed header and needs no WAL lookup), `journal_size_limit=64MB`,
  `wal_autocheckpoint=0`.
- **Compile options:** `-O3 -march=native` with `THREADSAFE=2`, `DEFAULT_MEMSTATUS=0`, `DEFAULT_WAL_SYNCHRONOUS=1`,
  `DQS=0`, `LIKE_DOESNT_MATCH_BLOBS`, `MAX_EXPR_DEPTH=0`, `OMIT_DECLTYPE`, `OMIT_DEPRECATED`, `OMIT_PROGRESS_CALLBACK`,
  `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, `OMIT_AUTOINIT`, `OMIT_UTF16`, `OMIT_TRACE`, `OMIT_COMPLETE`, `OMIT_GET_TABLE`,
  `OMIT_TCL_VARIABLE`, `USE_ALLOCA`, `ENABLE_QPSG`, `HAVE_FDATASYNC`, `HAVE_USLEEP`.

**Requests**
- **HTTP parsing on raw bytes.** It reads only the request line, `Content-Length`, `Authorization`, `Connection` and
  `Transfer-Encoding`, and decodes chunked bodies. Status lines and headers are precomputed.
- **JWT verified in full on every request** (rule 5). One keyed `javax.crypto.Mac` is reused, the signature is
  compared in constant time, and the token must be HS256 with a valid `exp` (required) and `nbf`.

**JVM**
- `-XX:+UseSerialGC` (no GC threads competing for the core), a fixed 128 MB heap, `-XX:+AlwaysPreTouch`,
  `-XX:+UseCompactObjectHeaders`, `-XX:-UsePerfData`, and capped metaspace, code cache and direct memory. A connection
  is a 64-byte object. At 300,000 users and 31,800 req/s, a 115-second run had one young GC, of 6.8 ms.

## Local measurements

These were measured on Docker on an Apple Silicon laptop (Linux 7.0, arm64), not the benchmark box. The server was
limited to one core, 2 GB, no swap and `nofile=65535`. Load came from `mload`, a C replay of `bench/load.js` from
`c-iouring-moneebullah25/tools`, in the same network namespace. Each level was a 60 s ramp and a 90 s hold.

| Version | Users | req/s | p95 | p99 | Errors | Connects | Server CPU | RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| epoll + eviction | 102,000 | 10,780 | 6.3 ms | 22.7 ms | 0.011% | 589,982 | 28% | |
| io_uring | 102,000 | 10,780 | 3.6 ms | 4.7 ms | 0 | 102,000 | 22% | |
| io_uring | 200,000 | 21,200 | 3.9 ms | 5.1 ms | 0 | 200,000 | 27% | 284 MB |
| io_uring | 300,000 | 31,800 | 4.6 ms | 5.9 ms | 0 | 300,000 | 32% | 284 MB |
| io_uring, 5-minute hold | 300,000 | 31,750 | 4.2 ms | 5.5 ms | 0 | 300,000 | 31% | 313 MB |

**These numbers don't predict a score.** Over loopback, the load generator's CPU pays for much of the network work
that the server pays for on a real box. To calibrate that, two scored entries ran through the same harness:

| Entry | CPU per request here | Scored | Scored users |
|---|---:|---:|---:|
| `rust-iouring-kensac` | 12.7 µs | 25 µs | 310,000 |
| `java-uring-crux` | 19.5 µs | 43 µs | 158,000 |
| this entry, epoll + eviction | 26 µs | 61 µs | 102,000 |
| this entry, io_uring | 12.7 µs | not yet | not yet |

**Memory at 300,000 users:** 1,121 MB of kernel socket memory and 313 MB of JVM RSS, plus reclaimable page cache from
the database. If every client disconnects in the same instant, the kernel charges about a page per socket for the queued
FINs until the server reads them. When `mload` exited at 300,000 users, that passed the container's 2 GB limit and got
the server killed, after the hold. With a 4 GB limit the same burst passed and the server stayed healthy with the same
RSS. A gradual ramp-down shouldn't produce this burst, though that wasn't tested here.

**Durability:** three runs of `kill -9` during about 24,000 writes per second lost none of about 1 million acknowledged
writes, `integrity_check` was ok each time, and the WAL stayed under 20 MB.

## License

MIT, under the repo's [license](../../LICENSE).
