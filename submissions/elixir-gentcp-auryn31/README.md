# Elixir + raw gen_tcp + SQLite (esqlite)

**Direct on `0.0.0.0:80`, no Nginx.** About 400 lines of Elixir in three modules and no web framework.

| | |
|---|---|
| Language | Elixir 1.20.5 on Erlang/OTP 29.1.1 |
| HTTP | hand-written HTTP/1.1 keep-alive server on `:gen_tcp`; one process per connection; requests parsed with OTP's built-in C parser (`:erlang.decode_packet`) |
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
- **One scheduler.** `+S 1:1 +SDcpu 1:1 +SDio 1 +A 1`: one normal scheduler plus one dirty-IO scheduler for SQLite steps. `+hmbs 1024` collects response binaries sooner on mostly idle connections. `start.sh` raises the descriptor soft limit to the hard limit.

## Rules

- **No caching across requests.** Nothing but the prepared statements and the JWT secret is kept in memory; every read queries SQLite and every authenticated request re-verifies its token's signature and expiry (`exp`, `nbf`, HS256 only).
- Validation counts Unicode code points and trims with JavaScript's `String.trim()` whitespace set; invalid JSON or UTF-8 is a `400 malformed JSON body`.
- Request bodies over 16 KiB close the connection (outside the spec).

## Local numbers (not the official box)

`bash test/run.sh submissions/elixir-gentcp-auryn31` passes 42/42. On an Apple M4 Pro with the same one-scheduler flags, k6 and the server sharing the machine, with think times scaled 10x (10,000 VUs, about 100,000 users of request rate): 8.1k req/s, p95 2.3 ms, p99 22.6 ms, 0 errors. The Erlang+Cowboy entry on the same setup: p95 198 ms, 1.6% errors. The load generator was the limit beyond that. These are not official scores.
