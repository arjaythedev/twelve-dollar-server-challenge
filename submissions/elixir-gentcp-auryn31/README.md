# Elixir + raw OTP sockets + SQLite (esqlite)

**Direct on `0.0.0.0:80`, no Nginx.** About 400 lines of Elixir in three modules and no web framework.

| | |
|---|---|
| Language | Elixir 1.20.5 on Erlang/OTP 29.1.1 |
| HTTP | hand-written HTTP/1.1 keep-alive server on OTP's `:socket` NIFs; one process per connection; requests parsed with OTP's built-in C parser (`:erlang.decode_packet`) |
| Database driver | [esqlite](https://github.com/mmzeeman/esqlite) 0.8.8 (NIF), built against the pinned SQLite 3.53.4 amalgamation |
| JSON / JWT | OTP's `:json` and `:crypto`: no hex packages |
| Nginx | **no**, direct |

## Build and run

`install.sh` (root) builds Erlang/OTP 29.1.1 from SHA-256-checked source into `/opt/erlang/29.1.1` and unpacks the
official Elixir 1.20.5 release (SHA-256-checked) into `/opt/elixir/1.20.5`. `build.sh` downloads the pinned esqlite
commit and the SQLite amalgamation (both SHA-256-checked), compiles the NIF with `cc` and the app with `elixirc`.
No Mix, no hex, no prebuilt binaries. `start.sh` runs the server in the foreground. On macOS it uses an existing
Elixir/OTP install.

## What makes it fast

- **No framework.** Each connection is one BEAM process in a `recv` loop. Pipelined requests and partial reads are handled in the same loop; idle connections close after 75 s.
- **One native call per read.** SQLite builds each response as a single JSON column (`json_object` / `json_group_array`, key order as in the spec), so the server never walks rows in Elixir. Strings are escaped by SQLite.
- **One database owner.** A single `GenServer` holds the connection and persistent prepared statements; reads and writes are serialized through it. `locking_mode=EXCLUSIVE`, WAL, `synchronous=NORMAL`, 64 MiB page cache, 256 MiB mmap.
- **Writes are durable before the answer.** `INSERT ... RETURNING` is stepped to completion (committed) before the 201; likes finish their autocommit transaction first.
- **`sqlite3_step` runs on the normal scheduler.** `build.sh` flips one flag in the esqlite NIF table (`step` from dirty-IO to normal; it greps to check the patch applied). With the page cache and mmap warm a step takes microseconds, so the hop to a dirty-IO thread (a second OS thread fighting for the one core) cost more than the work. Locally this took `/feed` from 25.7k to 31k and `/posts/:id` from 33k to 49k req/s. The cost: a slow checkpoint fsync stalls the single scheduler, which the database owner would have stalled on anyway.
- **One scheduler.** `+S 1:1 +SDcpu 1:1 +SDio 1 +A 1`: one normal scheduler plus one dirty-IO scheduler for SQLite steps. `+hmbs 1024` collects response binaries sooner on mostly idle connections. `start.sh` raises the descriptor soft limit to the hard limit.

## Rules

- **`:socket` instead of `:gen_tcp`.** `gen_tcp` goes through the inet port driver: every recv and send is a port command plus a reply message. The `:socket` module calls the syscalls directly from the connection process. Locally `/posts/:id` went from 46k to 51k req/s and `/feed` was unchanged; on the 10,000-VU mixed run p95 fell from 2.7 to 1.8 ms and p99 from 13 to 6 ms.
- **No caching across requests.** Nothing but the prepared statements and the JWT secret is kept in memory; every read queries SQLite and every authenticated request re-verifies its token's signature and expiry (`exp`, `nbf`, HS256 only).
- Validation counts Unicode code points and trims with JavaScript's `String.trim()` whitespace set; invalid JSON or UTF-8 is a `400 malformed JSON body`.
- Above 30,000 open connections every response carries `connection: close`, so descriptors (`LimitNOFILE=65535`) never run out; below that, connections stay alive.
- Request bodies over 16 KiB close the connection (outside the spec).

## Tried and left out (measured on an Apple M4 Pro)

- **Group commit** (drain the mailbox, run several writes in one transaction, reply after `COMMIT`): no gain on likes or creates (9.7k vs 10.4k and 12.9k vs 12.4k req/s). The cost is the index and page writes, not the commit. Removed.
- **`+sbwt none` (no scheduler busy-waiting):** no gain on a 14-core machine, so left out. It may matter on one core; I could not test that here.
- **JWT checking** costs 3.2 µs per request (HMAC 0.8 µs, claims decode 0.9 µs), so it is not worth optimizing.
- **Larger `wal_autocheckpoint`** (10,000 pages instead of 1,000): likes 10.3k → 14.5k and creates 12.9k → 14.5k req/s, but reads lost 10% (`/posts/:id` 55.9k → 50.1k, `/feed` 36k → 33k) because WAL pages are read with `pread`, not through the mmap. The load test does about twelve reads per write, so the two cancel out. At 50,000 pages reads halve. Left at the default.
- **The GenServer hop to the database owner** costs under 1.5 µs per call (a `SELECT 1` round trip is 2.0 µs in total, a `/posts/:id` query 4.0 µs), so a bare send/receive loop would not be worth its code.

## Local numbers (not the official box)

`bash test/run.sh submissions/elixir-gentcp-auryn31` passes 42/42. On an Apple M4 Pro with the same one-scheduler flags, k6 and the server sharing the machine, with think times scaled 10x (10,000 VUs, about 100,000 users of request rate): 8.4k req/s, p95 1.8 ms, p99 6 ms, 0 errors. The Erlang+Cowboy entry on the same setup: p95 198 ms, 1.6% errors. The load generator was the limit beyond that. These are not official scores.
