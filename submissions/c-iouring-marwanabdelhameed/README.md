# c-iouring-marwanabdelhameed

C, io_uring, SQLite in-process. **Direct** on `HOST:PORT` (`0.0.0.0:80` on the benchmark box), no Nginx.

## Constraint card

Rules and SPEC as they bind this entry (README rules 1 to 11, SPEC.md).

| Rule | What it bans | What this entry does |
|---|---|---|
| 2 | Any database but the SQLite file at `$SQLITE_PATH`; cache servers; external services | SQLite 3.50.4 compiled into the binary, used in-process |
| 3 | Adding or changing tables, columns, indexes or triggers | Schema untouched. Only the SQL of SPEC.md's reference queries is used |
| 4 | Failing `test/test.sh`; inexact bytes | 42 of 42 checks, see Results |
| 5 | Caching across requests: response caches, query-result caches, in-memory table copies, remembered JWT verifications | Every request runs a SQLite statement and verifies the token's HMAC. Prepared statements are reused, which rule 5 allows. The only per-process state is connection buffers and the HMAC key schedule (the keyed pad states hold no token data) |
| 6 | `synchronous=OFF`, `journal_mode=OFF/MEMORY`, an in-memory database; a 201 before the commit | `journal_mode=WAL`, `synchronous=NORMAL`, checked at startup (the server exits otherwise). Group commit: all writes of one event batch share one `BEGIN IMMEDIATE ... COMMIT`, and every 201 or like response is sent only after that commit returned. A failed commit turns each held response into a 500 |
| 7 | Hard-coded responses, detecting the load test | None |
| 8 | More than 2 GB RAM, swap | Per idle connection: one 2 KB input buffer and a small struct. No output buffer while idle |
| 9 | (optional Nginx) | Direct on `HOST:PORT`. Keep-alive connections stay open until the client closes them, so the 65 s Nginx idle rule holds |
| 10 | Prebuilt binaries; unpinned dependencies; slow start | `build.sh` downloads two pinned sources and checks their SHA-256 (liburing 2.5, SQLite 3.50.4). The server answers `/health` on the first poll after `start.sh` |
| 11 | Kernel parameters, CPU pinning | Only in-process settings: SQLite pragmas, `RLIMIT_NOFILE` raised to its hard limit |

The SHA-256 values were recorded from the first download (trust on first use).

## How it works

- One thread, one event loop. Per batch of completions the loop runs the requests, then commits once, then sends.
- io_uring: multishot accept on the listener, one `recv` or `send` in flight per connection, `submit_and_wait` per batch.
- Two SQLite connections to the same WAL file. The read connection serves `GET` routes and the existence check of a like, so it only sees committed rows. The write connection holds the open group-commit transaction.
- JWT: HS256 only, signature checked with a constant-time compare, then `exp`, `nbf`, `sub` and `username`.
- JSON: a strict validating scanner (`src/json.c`), `\u` escapes and surrogate pairs decoded.
- Response bodies are built with a hand-written escaper in the SPEC's key order.
- Trimming follows `String.prototype.trim` whitespace. The 500-character limit counts code points.

### epoll fallback

If `io_uring_setup` fails, the same request code runs on epoll (level-triggered). The server logs which backend it chose
on stderr. 

## Versions

| Part | Version |
|---|---|
| Language | C (gnu11), gcc from `build-essential` |
| Ring library | liburing 2.5, built as a static library |
| SQLite | 3.50.4 amalgamation, `SQLITE_THREADSAFE=0`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_DQS=0` |
| Pragmas | WAL, `synchronous=NORMAL`, `busy_timeout=5000`, read connection `mmap_size=512 MB`, `query_only=1` |

## Results

`test/test.sh`: 42 of 42 passed with the io_uring backend on Ubuntu 24.04, 1 vCPU, 2 GB.
