# AGENTS.md: python-stdlib-shawal-mbalire

Handoff notes. The source is the source of truth; this file holds the rules, the why, the verified
numbers and what's next. Read it in full before touching code. Started 2026-10-16; last handover
2026-10-17 ~01:00.

**Where things stand (2026-10-17):** the server is a hand-written epoll loop nested in asyncio
(`adapters/epoll_http.py`), adopted after a 130k A/B of three servers. On the fixed bench it passes
**180k users in 2-minute holds** (CPU 0 at 92%) and costs ~11% more CPU per request than #84 PyPy at
130k. **No 5-minute hold has been run yet**: that is the next step and the only number to claim.
README.md is written with the 2-minute numbers, labelled as such. Committed and pushed to `fork`,
branch `python-stdlib-shawal-mbalire` (2026-10-17); PR #118.

## Goal and owner's rules

A $12 Server Challenge entry (`../../README.md`, `../../SPEC.md`): 1 vCPU, 2 GB RAM, score = most
concurrent k6 users held for 5 minutes (p95 < 500 ms, p99 < 1 s, < 1% errors).

This is the owner's **experiment after PR #41** (`../python-fastapi-granian-shawal-mbalire`, FastAPI +
Granian, held 120k): serve the same five endpoints with the **Python standard library only**, to see
how close plain CPython gets to #84 (PyPy + raw epoll) and #71 (Ruby). Rules (all the owner's):

- **Standard library only.** stdlib `asyncio` event loop (no uvloop) with an epoll loop nested in it, a hand-written
  HTTP/1.1 parser, stdlib `json` (not orjson), `sqlite3`, `hmac`/`hashlib`. No `requirements.txt`. Any
  third-party package needs a measured reason and the owner's OK.
- **The latest Python and its features**: CPython 3.15.0, **compiled from python.org's source by
  `build.sh`** (owner, 2026-10-17: rule 10 says "built from source on the box … don't ship prebuilt
  binaries", which python-build-standalone broke). clang 19 from apt, PGO + ThinLTO, the tail-call
  interpreter, linked statically against SQLite 3.53.1 compiled from sqlite.org's amalgamation; both
  tarballs pinned by SHA-256 in `infra/ops.py` (Python's matches its `.sigstore` digest). The server runs
  from `.venv/` (no pip, no packages; the owner wanted a venv). The numbers below until the next
  entry in "Verified numbers" were measured on python-build-standalone 3.15.0 (release 20261013). Modern syntax: PEP 695
  (`type` aliases, `class Ok[T]`), `match`, slots dataclasses, deferred annotations (so ≥ 3.14 locally).
- **SQLite builds the response JSON**; Python only joins byte strings.
- **Hexagonal architecture** (the owner's `hexagonal-architecture` skill): domain / adapters / infra /
  tests; only the driving adapter differs from the FastAPI entry.
- **justfile**, **no uv**, **no shell scripts** (the required `.sh` files stay one line each),
  **spelled-out names**, **honest claims** (every number from the fixed `just bench linux`, saying so).
- **Own folder, own branch, own PR.** Copied (not imported) from the FastAPI entry. Don't commit,
  push or open a PR until the owner asks. Ask before A/B experiments that change the design.

## Challenge rules (summary of SPEC.md)

SQLite in-process at `$SQLITE_PATH`, schema unchanged. No caching across requests; every JWT
verified. Writes durable before the response (WAL + `synchronous=NORMAL`; group commit, each response
waits). All 42 checks of `test/test.sh` byte for byte. `install.sh` / `build.sh` / `start.sh` (env
`SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`), healthy within 60 s. Served **direct** on `0.0.0.0:80`.

## State of the tree

Committed on branch `python-stdlib-shawal-mbalire` (from `origin/main`, only this folder), pushed to
`fork` 2026-10-17; PR #118 opened the same day. The FastAPI entry's bench builds this folder from whatever is checked
out (`REFS` name `stdlib`, `sha: None`), so check out this branch's folder there (or keep it in the
working tree) before benching it. The bench changes (REFS `stdlib`, `STDLIB_DOCKERFILE`, REFS `"env"`)
are committed on the FastAPI branch.

Tested 2026-10-17 (epoll server): `just test` 78 pass and `just check` 42/42 on the Mac (3.14, kqueue
fallback) and in a Linux container with 3.15.0 (epoll, TCP_DEFER_ACCEPT), the server exiting cleanly on
SIGTERM. The full CI `test/run.sh` (clean Ubuntu 24.04, downloads 3.15.0) on the epoll server,
2026-10-17: 42/42.
Again after the switch to compiling CPython from source (2026-10-17 03:07): 42/42; install + build +
start + tests took 2 min 46 s in the Colima VM (8 CPUs, `make -j8`, PGO profile run ~30 s). Expect far
longer on a 1-vCPU droplet. **Not benched yet on the source build**: A/B it against the standalone build
(`just bench snapshot`) before quoting numbers for it.

## Layout

```
main.py     composition root: compose() -> Application(dispatcher, resources); serve() until SIGTERM/SIGINT
domain/     copied from the FastAPI entry; models.py uses PEP 695 syntax
adapters/   http1 (pure parser + response heads), stdlib_http (Dispatcher, HttpConnection, HttpServer),
            listener (listening socket + accept gate), epoll_http (the server), json_codec (strict stdlib json), jwt_hs256, sqlite_posts,
            descriptor_budget, clock, span_recorder, cprofile_profiler
infra/      config.py (now with HOST/PORT), ops.py (build: pinned interpreter; start: exec main.py)
tests/      stdlib unittest; fakes in fixtures.py; test_http1 (parser), test_stdlib_http (routing,
            protocol with a fake transport), test_listener (the epoll server
            on loopback: keep-alive, pipelining, commit waits, strained close, big writes, the gate)
```

## Decisions that are easy to break

- **Coroutines are driven by hand, not wrapped in Tasks** (`HttpConnection._process`): `coroutine.send(None)`;
  `StopIteration` = done (every read without an open group commit). A write yields its batch's future;
  `_resume` runs from its done callback. Pipelined requests behind a suspended one wait in `_buffer`, so
  answers stay in order. Don't await anything that isn't an asyncio future in the request path (a bare
  yield works but costs a loop pass).
- **Group commit, EXCLUSIVE locking, PASSIVE checkpoint every 1 s**: unchanged from the FastAPI entry
  (`adapters/sqlite_posts.py`); its Tried table explains why.
- **Descriptor budget** (`adapters/descriptor_budget.py`): unchanged: sockstat count, reserve 4,096,
  50 ms. While strained, responses say `Connection: close` and the transport closes after the write.
- **Response heads** are prebuilt per (status, close) with a Date refreshed once a second (`ResponseHeads`).
- **Idle sweep**: every 120 s, connections with no request since the previous sweep close (so idle
  ones live 120–240 s; SPEC wants ≥ 65 s).
- **Parser limits**: head 16 KiB (431), body 1 MiB (413), both framings at once → 400, garbage → 400
  and close. `Expect: 100-continue` gets one `100 Continue`. HTTP/1.0 closes unless keep-alive.
- **JSON is strict** (`adapters/json_codec.py`): NaN/Infinity, non-UTF-8 and lone surrogates are
  "malformed JSON body", as orjson made them in the FastAPI entry.
- **The HTTP server is a hand-written epoll loop nested in asyncio's** (`adapters/epoll_http.py`, the
  owner's choice 2026-10-17 after the 130k A/B below). asyncio watches one descriptor, the epoll
  instance; `_poll` reads and writes ready sockets with `os.read`/`os.write` through an `EpollTransport`
  per connection, which gives `HttpConnection` (parsing, pipelining, the hand-driven coroutines) the
  transport calls it makes. Timers, group commit and checkpoints stay on asyncio. On a Mac the same loop
  runs on `selectors` (kqueue), for the tests. A closed descriptor leaves epoll by itself (no unregister).
- **Listener** (`adapters/listener.py`): backlog 4096, SO_REUSEADDR, TCP_DEFER_ACCEPT 2 s (a connection
  wakes us only with its first request, which is answered inside the accept loop; a retired one closes
  without ever entering epoll), TCP_NODELAY on the listener (Linux copies it to accepted sockets).
  `listener._accept()` returns the bare descriptor: no socket object per connection.
- **Accept gate** (`ListeningServer`): accepting stops while `len(connections)` ≥ the descriptor limit − 64
  and resumes as connections close; an EMFILE anyway pauses accepting 50 ms and logs one line, no
  traceback (asyncio's own server paused 1 s and logged tracebacks that failed under EMFILE). Stress
  test (Linux container, 3.15, nofile 1,000, 1,500 clients, ~72k requests): asyncio's server fell into
  EMFILE (480k "Logging error" lines, client unfinished at 2 min); the gated servers answered all, empty log.
- **Rejected 2026-10-17** (measured, then removed from the tree): asyncio's `loop.create_server` and
  an own accept path with asyncio transports (`_make_socket_transport`, first answer on the bare
  socket). Numbers in "The three HTTP servers at 130k" below.

## Commands

`just test` and `just check` after every change (`just --list` for the rest). The bench lives in the
FastAPI entry: `just bench ...` here forwards to its justfile (see its AGENTS.md "Commands"), e.g.
`just bench linux 40000 --submission stdlib`, `just bench entries 40000 ours stdlib pypy-pranavds ruby-cicloid --rounds 3`.
The stdlib entry builds with a layered Dockerfile (`STDLIB_DOCKERFILE`, REFS `"layered": True`, added
2026-10-16 after a 16-minute stall: GitHub served the 35 MB interpreter at ~18 KB/s), so the compiled
interpreter is cached until `install.sh` or `infra/ops.py` changes. Then the PGO compile runs again (see
"State of the tree" for how long). If GitHub is slow,
`--no-build` reuses the last images (check their dates against your last code change).

## Verified numbers (fixed bench)

**Epoll server, limit search** (`just bench ladder 140000 --upper 200000 --resolution 10000 --submission stdlib`,
2-minute holds, 60 s ramp; image built 00:33 from this tree), 2026-10-17 00:33–00:49. Every run is in
`../python-fastapi-granian-shawal-mbalire/build/linuxbench/results.jsonl` (`just bench results 4` there):

| Users | Pass | server µs/request | `core0_util` | p95 ms | p99 ms | errors | `listen_overflows` | peak sockets | peak MiB |
|---|---|---|---|---|---|---|---|---|---|
| 140k | yes | 56.0 | 66% | 8.5 | 24 | 0 | 0 | 62.0k | 691 |
| 170k | yes | 58.8 | 84% | 28.7 | 72 | 0 | 0 | 62.3k | 751 |
| 180k | yes (`limited_by: cpu`) | 60.4 | 92% | 105 | 207 | 0 | 0 | 63.1k | 779 |
| 190k | no | 67.0 | 79% | 813 | 13,510 | 0.13% | 838,390 | 65.8k | 963 |

The 190k run is flagged `limited_by: generator` (generator 6.3 s late), so it doesn't cleanly show the
server's limit; but 180k already ran CPU 0 at 92%, so ~180k is the 2-minute edge. Start the 5-minute
holds at 170k.

For comparison, PR #41 (FastAPI + Granian) holds 120k for 5 minutes and fails 140k. 2-minute ladder runs
overstate the limit; only a `--duration 300` hold counts as the entry's number.

**The three HTTP servers at 130k, alternating** (`just bench entries 130000 stdlib stdlib-accept stdlib-epoll
pypy-pranavds --rounds 3 --no-build`, images built 22:58 from this tree, 60 s ramp, 120 s hold,
2026-10-16 23:08–23:47, log `.../entries/20261016-234736.json`; `stdlib-accept`/`stdlib-epoll` were REFS
entries setting `HTTP_SERVER`, both removed with that setting). 0 errors in every run:

| Server | Pass | server µs/request | `core0_util` | p95 ms | p99 ms | `listen_overflows` | peak sockets | steady MiB |
|---|---|---|---|---|---|---|---|---|
| `asyncio` (create_server, removed) | 2/3 | 75.9–85.0 (79.0) | 84–85% | 18.6–2,221 | 41–4,033 | 0, 0, 344,971 | 62.0k–69.6k | 328–403 |
| `accept` (a + gate, removed) | 3/3 | 70.3–72.7 (71.4) | 76–80% | 11.5–162 | 31–882 | 0, 46,881, 12,585 | 64.3k–66.4k | 327–339 |
| **`epoll` (c + gate)** | **3/3** | **57.4–59.5 (58.7)** | 63–65% | 8.0–11.1 | 24–63 | 0, 0, 0 | 61.9k–62.0k | 182–184 |
| #84 PyPy | 3/3 | 51.1–54.9 (52.7) | 56–59% | 8.3–9.3 | 14–17 | 0 | 25.2k | 115–117 |

- `epoll` is ~26% cheaper per request than `asyncio`, ~18% cheaper than `accept`, and ~11% behind #84. It
  also holds ~45% less memory (no transport, protocol state and 256 KiB read buffer per connection).
- `asyncio` failed again the same way as at 20:58: 69,624 sockets, EMFILE, asyncio's 1 s accept pause,
  345k overflows.
- `accept` passed, but in 2 runs its sockets reached ~66k: the gate (limit − 64) stopped accepting and the
  queue overflowed (12.6k and 46.9k, p99 up to 882 ms). Its per-connection cost is still too high to keep
  the count under the descriptor budget's 61,439. `epoll` never went above 62.0k sockets.

### History: the asyncio.Protocol server (create_server, removed 2026-10-17)

The numbers below were measured on the first server design. Keep them as history; they are not the
current build's numbers.


**Entries at 40k, alternating** (`just bench entries 40000 ours stdlib pypy-pranavds ruby-cicloid --rounds 3`,
20 s ramp, 120 s hold, 2026-10-16 19:10–19:41, log `../python-fastapi-granian-shawal-mbalire/build/linuxbench/entries/20261016-194136.json`).
All 12 runs passed with 0 errors and ~507.4k requests each. Range (mean):

| Entry | server µs/request | `core0_util` | p95 ms | p99 ms | peak MiB |
|---|---|---|---|---|---|
| #71 Ruby | 34.3–39.6 (36.2) | 17–18% | 1.3–1.4 | 3.3–3.5 | 510–555 |
| **stdlib (this entry)** | **38.8–39.4 (39.2)** | 18% | 1.4 | 3.1–3.3 | 772–847 |
| #84 PyPy | 38.0–42.0 (39.4) | 20% | 1.2–1.5 | 3.1–3.2 | 611–736 |
| FastAPI + Granian (PR #41, 3.11 + Rust tasks) | 47.1–49.1 (48.4) | 20–21% | 1.6–1.7 | 3.3–3.8 | 978–1,029 |

So at 40k: ~19% cheaper per request than PR #41, level with #84 (means within 0.5%, ranges overlap),
~8% behind #71, with the lowest spread of the four. CPU 0 (kernel included) is 18% vs #84's 20%.
Peak memory includes the database's page cache charged to the cgroup; steady RSS was ~213 MiB in
the single run below. **Not re-run on the epoll server yet** (next steps).

**130k, alternating** (`just bench entries 130000 stdlib pypy-pranavds ours --rounds 3 --no-build`, 60 s
ramp, 120 s hold, 2026-10-16 20:48–21:17, log `.../entries/20261016-211746.json`; images from 19:07):

| Entry | Pass | server µs/request | `core0_util` | p95 ms | p99 ms | Connects | peak MiB |
|---|---|---|---|---|---|---|---|
| #84 PyPy | 3/3 | 49.3–49.7 (49.5) | 55% | 8.1 | 12.9–13.5 | 1.68M (keep-alive budget 25k) | 662–704 |
| FastAPI + Granian (PR #41) | 3/3 | 62.1–63.5 (62.6) | 68–69% | 8.8–9.5 | 29–35 | 1.08M | 1,435–1,517 |
| **stdlib** | **2/3** | **73.4–76.1 (75.1)** | 80–83% | 15–1,151 | 40–2,032 | 1.08M | 824–1,082 |

The stdlib entry is the cheapest Python at 40k and the most expensive at 130k: 39 → 75 µs (×1.9), against
×1.3 for PR #41 and #84. What changes past ~61k open sockets is that ~1.0M responses close their
connection and the client reconnects, so the per-connection cost dominates: asyncio builds a socket object,
a transport and a protocol, registers and unregisters the selector, and schedules connection_made /
connection_lost with call_soon for every one. #84 accepts 1.6× more connections at 49 µs (raw epoll, accept4).

The failed run (20:58, samples `20261016-205829.jsonl`): at the start of the hold CPU 0 hit 100%, TCP
sockets reached 69,624 (> 65,535), the server hit **EMFILE**, asyncio paused accepting for 1 s
(its EMFILE handling), and the accept queue overflowed (30,610 `listen_overflows`, generator 171 ms
late). The server log holds 4,045 "--- Logging error ---" entries: logging the EMFILE traceback opens
source files (linecache), which fails too. The other two runs stayed at ~61.8k sockets, 0 overflows.

Single runs before that (not alternated; 40k users, 20 s ramp, 120 s hold, 3.15.0):

| When | Build | Pass | server µs/request | `core0_util` | p95 / p99 ms | Connects | Steady / peak MiB |
|---|---|---|---|---|---|---|---|
| 2026-10-16 18:59 | after the blocking-flag fix | yes, 0 errors | 43.9 | 18% | 1.4 / 3.4 | 40,000 | 213 / 557 |
| 2026-10-16 18:53 | before it (bug below) | no, 18.6% errors | 52.1 | 24% | 1.5 / 3.2 | 135,694 | 187 / 493 |


## Bugs found

- **`await wasn't used with future`** (fixed 2026-10-16): driving coroutines by hand, the yielded future's
  `_asyncio_future_blocking` flag must be reset, as `asyncio.Task` does; otherwise the second request
  awaiting the same group-commit batch raised RuntimeError → 500, the generator reconnected (136k
  connects, 18.6% errors at 40k). Regression test `test_many_requests_wait_for_the_same_commit`. 500s
  are now logged (`internal_error()`), since the swallowed exception hid it.

## Tried and rejected

| Idea | Result |
|---|---|
| `asyncio.Protocol` on `loop.create_server` (the first design) | 130k: 75.9–85.0 µs, 2/3 and 2/3 passes in two alternating batches; its failures went past 65,535 sockets, hit EMFILE, paused accepting 1 s, 30k–345k accept-queue overflows. Asyncio's per-connection work (Task, waiter, transport, call_soon x2) dominates once ~1M responses a run close their connection. Removed 2026-10-17 |
| Own accept path, asyncio transports (`_make_socket_transport`, first answer on the bare socket, gate) | 130k: 70.3–72.7 µs, 3/3, but 2 runs passed 66k sockets and overflowed the queue (12.6k, 46.9k; p99 up to 882 ms). Removed 2026-10-17 |

## Next steps (handover 2026-10-17 ~01:00)

1. Done: the three-server A/B, the switch to epoll, the ladder (180k passes 2 minutes), README.md.
2. **5-minute holds**: `just bench linux 170000 --duration 300 --submission stdlib --repeat 3` (then 180k
   if all pass; 160k if any fails). That is the number to claim; record it here and replace the README's "Limit search" caveat with it.
3. **Entries comparison on the epoll build** at 40k (`just bench entries 40000 stdlib pypy-pranavds
   ruby-cicloid ours --rounds 3`): the 40k table above is from the old asyncio server.
4. Done 2026-10-17: CI's `test/run.sh` in a clean Ubuntu 24.04 container (context `build/ci`: copy
   `test/`, `schema.sql`, `seed/feed.db` and this folder into `build/ci/repo/`), 42/42.
5. Runtime knobs to A/B (owner's list, ask first): `PYTHON_JIT=1`, `PYTHONMALLOC=mimalloc`. A REFS entry
   may carry `"env": (...)` to bench the same folder with extra environment (see `bench/linux.py`).
6. Keep README.md in step with the numbers (fixed bench only). PR #118 is open with the 2-minute numbers
   (labelled); update its description once the 5-minute holds are in.
7. Possible cleanup (owner's OK first): the stopped Colima profiles `bench-server` and `bench-gen`.
