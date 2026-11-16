# c-iouring-marwanabdelhameed

C, io_uring, SQLite in-process. **Direct** on `HOST:PORT` (`0.0.0.0:80` on the benchmark box), no Nginx.

## Constraint card

Rules and SPEC as they bind this entry (README rules 1 to 11, SPEC.md).

| Rule | What it bans | What this entry does |
|---|---|---|
| 1 | (allows any language, any number of processes or threads) | Two processes: the server, and one helper process it forks at startup to checkpoint the WAL (see "How it works"). The helper exits when the server does |
| 2 | Any database but the SQLite file at `$SQLITE_PATH`; cache servers; external services | SQLite 3.50.4 compiled into the binary, used in-process. The helper opens the same file through the same library |
| 3 | Adding or changing tables, columns, indexes or triggers | Schema untouched. Only the SQL of SPEC.md's reference queries is used. `train/schema.sql`, used by the build's training step on a scratch database, is a copy of `schema.sql` |
| 4 | Failing `test/test.sh`; inexact bytes | 42 of 42 checks, see Results |
| 5 | Caching across requests: response caches, query-result caches, in-memory table copies, remembered JWT verifications | Every request runs a SQLite statement and verifies the token's HMAC. Prepared statements are reused, which rule 5 allows. The only per-process state is connection state, the HMAC key schedule (the keyed pad states hold no token data) and the ring of receive buffers, which holds a request's bytes only until it has been parsed |
| 6 | `synchronous=OFF`, `journal_mode=OFF/MEMORY`, an in-memory database; a 201 before the commit | `journal_mode=WAL`, `synchronous=NORMAL` on every connection, checked at startup (the server exits otherwise). Group commit: all writes of one event batch share one `BEGIN IMMEDIATE ... COMMIT`, and every 201 or like response is sent only after that commit returned. A failed commit turns each held response into a 500. Only the checkpoint, the copy of WAL pages into the database file, moves to the helper process, which runs it with the same `synchronous=NORMAL` |
| 7 | Hard-coded responses, detecting the load test | None |
| 8 | More than 2 GB RAM, swap | About 500 bytes of resident memory per idle connection averaged over 20,000, and about 75 bytes for each connection beyond the first thousands, plus a fixed 8 MB pool of receive buffers (no per-connection input buffer while idle). Peak resident memory stays under 80 MB at 25,000 requests per second |
| 9 | (optional Nginx) | Direct on `HOST:PORT`. Keep-alive connections stay open until the client closes them, so the 65 s Nginx idle rule holds |
| 10 | Prebuilt binaries; unpinned dependencies; slow start | `build.sh` downloads two pinned sources and checks their SHA-256 (liburing 2.5, SQLite 3.50.4) and builds everything from source on the box, including a profile-guided build that trains itself (below). No profile data or binary is committed. The server answers `/health` on the first poll after `start.sh` |
| 11 | Kernel parameters, CPU pinning | Only in-process settings: SQLite pragmas, `RLIMIT_NOFILE` raised to its hard limit, the helper process the server starts itself, and io_uring and socket setup inside the process. No sysctl, no pinning |

The SHA-256 values were recorded from the first download (trust on first use).

## How it works

- One thread, one event loop. Per batch of completions the loop runs the requests, then commits once, then sends.
- io_uring: multishot accept on the listener, one multishot `recv` per connection that draws buffers from a provided buffer ring, one `send` in flight per connection, `submit_and_wait` per batch. A request that fits in one buffer is parsed in place. If it is split across buffers, the bytes are appended to a small owned buffer until the request is whole, and a connection with nothing buffered owns no input buffer at all.
- Two SQLite connections to the same WAL file. The read connection serves `GET` routes and the existence check of a like, so it only sees committed rows. The write connection holds the open group-commit transaction.
- WAL checkpoints run in a helper process. The server sets `wal_autocheckpoint=0` on its write connection and forks the helper before it touches SQLite. The helper opens its own connection and runs a PASSIVE checkpoint every 250 ms. A checkpoint ends in `fsync`, and inside the request loop that one call held the thread for about 11 s of every 60 s at 21,000 requests per second; in the helper it blocks only the helper. The helper dies with the server (`PR_SET_PDEATHSIG`).
- JWT: HS256 only, signature checked with a constant-time compare, then `exp`, `nbf`, `sub` and `username`.
- JSON: a strict validating scanner (`src/json.c`), `\u` escapes and surrogate pairs decoded. Response bodies are built in the SPEC's key order, integers by hand and strings escaped with a 16-byte SSE2 scan that copies each run without an escape in one `memcpy`.
- Trimming follows `String.prototype.trim` whitespace. The 500-character limit counts code points.

### Build: profile-guided, trained by `build.sh` itself

`bash build.sh` runs three steps, all from source on the box, as a normal user:

1. builds an instrumented server (`-fprofile-generate`, SQLite included);
2. `train/train.sh` starts it on `127.0.0.1:18080` against a scratch database made from `train/schema.sql` (2,000 users, 40,000 posts, about 120,000 likes), sends the SPEC's request mix with `curl` (feed, single post, likes, creates, with HS256 tokens made by `openssl`), and stops it, which writes the profile files under `pgo/`;
3. rebuilds with `-fprofile-use -fprofile-partial-training`.

`install.sh` adds `sqlite3`, `curl` and `openssl` for step 2. If a tool is missing or any step fails, the build falls back to the same code without the profile, so training never makes a build fail. The whole build takes about 80 s on 1 vCPU. `bin/`, `deps/` and `pgo/` are not committed.

### Compile-time flags

`build.sh` with no `EXP_CFLAGS` in the environment uses `-DV_CKPT_PROC -DV_MULTISHOT_RECV -DV_JSON_SPEC` plus the profile-guided steps above. The other `V_*` macros in `src/` (socket options, zero-copy send, header templates, a static SQLite heap, huge pages, registered files, deferred task run and others) are alternatives that were measured and left off. Setting `EXP_CFLAGS` to a list of `-DV_*` flags, or to an empty string, builds exactly that and skips the profile steps.

### epoll fallback

If `io_uring_setup` fails, the same request code runs on epoll (level-triggered). The server logs which backend it chose
on stderr.

## Versions

| Part | Version |
|---|---|
| Language | C (gnu11), gcc 13.3 from `build-essential` |
| Ring library | liburing 2.5, built as a static library |
| SQLite | 3.50.4 amalgamation, `SQLITE_THREADSAFE=0`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_DQS=0` |
| Pragmas | WAL, `synchronous=NORMAL`, `busy_timeout=5000`, `wal_autocheckpoint=0` on the write connection, read connection `mmap_size=512 MB`, `query_only=1` |

## Results

`test/test.sh`: 42 of 42 passed with the io_uring backend on Ubuntu 24.04, 1 vCPU, 2 GB.

Load test from a separate machine at a fixed request rate with a k6 version of `bench/load.js` (35 ms round trip, 60 s holds, the challenge's bar: p95 under 500 ms, p99 under 1 s, errors under 1%, held rate at least 98% of the target). Highest target where at least 2 of 3 runs passed:

| Build | Ceiling (req/s) |
|---|---|
| First version of this entry (one thread, checkpoint in the request loop) | 21,000 |
| This build | 25,500 (3 of 3 runs, p95 134 to 231 ms, p99 190 to 336 ms) |

These are request rates in a fixed-rate test, not the user counts of the challenge's own 5-minute hold.
