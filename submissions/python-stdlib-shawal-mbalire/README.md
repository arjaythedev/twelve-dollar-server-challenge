# Python standard library (CPython 3.15, epoll)

| | |
|---|---|
| Language | Python 3.15.0, compiled from python.org's source by `build.sh` (clang 19, PGO + ThinLTO, tail-call interpreter), pinned by SHA-256 |
| Framework | none: a hand-written HTTP/1.1 parser and router |
| Server | a hand-written `select.epoll` loop, nested in the standard `asyncio` event loop (no uvloop) |
| SQLite driver | the standard library `sqlite3` module, linked statically against SQLite 3.53.1 compiled from sqlite.org's source |
| JSON | SQLite's `json_object` renders every response; the standard `json` module only decodes request bodies and JWT parts |
| Third-party packages | **none** (no `requirements.txt`; the venv has no pip and only pins the interpreter) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

## Running it

```bash
sudo bash install.sh   # apt: ca-certificates, python3, make, clang/lld/llvm 19
bash build.sh          # compiles the pinned SQLite and CPython 3.15 into .python/, then makes .venv/
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` is a single `apt-get` line. `build.sh` and `start.sh` are one-line shims into `infra/ops.py`;
all of the logic is Python. On Linux, `build` downloads the CPython 3.15.0 and SQLite 3.53.1 source
tarballs, checks their SHA-256 and compiles them (no prebuilt binaries, per rule 10). Elsewhere, or with
`PYTHON=` set, it uses the local interpreter (3.14 or newer). Either way it then creates `.venv/`,
which `start` runs.

For development there is a [justfile](https://just.systems):

| Command | What it does |
|---|---|
| `just setup` | create `.venv/` on the local Python (3.14 or newer) |
| `just run` | serve on `127.0.0.1:3000` against `build/feed.db` |
| `just test` | unit tests (stdlib `unittest`) |
| `just check` | the official 42-check suite, exactly as CI runs it |
| `just bench ...` | the load bench, which lives in `../python-fastapi-granian-shawal-mbalire` (this entry is `--submission stdlib` there) |

## Layout

Hexagonal (ports and adapters), with the same domain as the author's FastAPI entry
(`../python-fastapi-granian-shawal-mbalire`), copied rather than imported. `domain/` holds the rules and the
five workflows as chains of named steps, and imports only the standard library. `adapters/` implements
the ports: the SQLite store, the HS256 verifier, the clock and the descriptor budget. It also holds the
HTTP driving adapter:

- `http1.py`: the parser and the prebuilt response heads;
- `stdlib_http.py`: the routes and one `HttpConnection` per client;
- `listener.py`: the listening socket and the accept gate;
- `epoll_http.py`: the event loop.

`infra/` reads settings and runs build/start, and `main.py` wires them together.

## How it works

- **One thread, one epoll loop.** asyncio watches a single descriptor, this server's epoll instance.
  When that is readable, the server reads and writes every ready socket with `os.read`/`os.write`.
  There is no transport, protocol object or scheduled callback per event. Timers, the group commit and
  the WAL checkpoint stay on asyncio.
- **Requests run without Tasks.** A workflow's coroutine is driven by hand (`coroutine.send`).
  - A read finishes inside the read event.
  - A write suspends on its group commit's future and resumes from that future's callback. Later
    pipelined requests wait behind it, so answers stay in order.
- **TCP_DEFER_ACCEPT** (as in PR #84): a new connection wakes the server only once its first request
  has arrived, so it is accepted, read and answered in one pass. A connection that will be closed after
  that answer never enters epoll.
- **Descriptors**: past about 61k open sockets, responses carry `Connection: close` and clients reconnect.
  The socket count comes from `/proc/self/net/sockstat`. On top of that, the server stops accepting
  64 descriptors short of the limit instead of hitting EMFILE.
- **SQLite**: one connection in `locking_mode=EXCLUSIVE`, WAL with `synchronous=NORMAL`, and a group
  commit per event-loop pass. Every write is answered only after its commit, and a PASSIVE checkpoint
  runs every second.

## Results

**Not a droplet score.** These numbers come from `just bench linux` on an Apple M-series Mac, whose
cores are faster than a droplet's vCPU.

They were measured on the same CPython 3.15.0 built by python-build-standalone (clang, PGO + LTO,
tail-call interpreter, SQLite 3.53.1), before `build.sh` compiled it from source with the same
features. They haven't been re-run on the source build yet.

The bench setup (in the FastAPI entry, declared in Terraform):
- The server gets CPU 0, 1792 MiB and no swap, with nofile 65535.
- Four load generators run PR #14's C client, which follows `load.js`.
- Network processing for the server's packets is steered onto CPU 0 (not loopback).

Users are k6-style virtual users; a run passes with p95 < 500 ms, p99 < 1 s and < 1% errors.

**Limit search, 2-minute holds** (2026-10-17). The challenge holds for 5 minutes, and those runs
haven't been done yet, so treat these as an upper bound:

| Users | Pass | server CPU µs/request | CPU 0 busy | p95 | p99 | errors |
|---|---|---|---|---|---|---|
| 140,000 | yes | 56.0 | 66% | 8.5 ms | 24 ms | 0 |
| 170,000 | yes | 58.8 | 84% | 28.7 ms | 72 ms | 0 |
| 180,000 | yes | 60.4 | 92% | 105 ms | 207 ms | 0 |
| 190,000 | no | 67.0 | 79% | 813 ms | 13.5 s | 0.13% |

The bench flagged the 190k run because the load generator itself fell 6.3 s behind.

**Against other entries, 130,000 users** (2026-10-16). The runs alternated, 3 rounds, with a 120 s hold:

| Entry | Pass | server CPU µs/request | CPU 0 busy | p99 |
|---|---|---|---|---|
| #84 (PyPy, raw epoll) | 3/3 | 51.1–54.9 (mean 52.7) | 57% | 14–17 ms |
| **this entry** | **3/3** | **57.4–59.5 (58.7)** | 64% | 24–63 ms |
| the same entry on `asyncio.Protocol` + `loop.create_server` | 2/3 | 75.9–85.0 (79.0) | 84% | 41 ms – 4.0 s |

In a separate alternating run at 130k on 2026-10-16, the author's FastAPI + Granian entry (PR #41)
cost 62.1–63.5 µs per request.
