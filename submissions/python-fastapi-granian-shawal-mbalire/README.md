# Python + FastAPI on Granian

| | |
|---|---|
| Language | Python 3.12.3 (Ubuntu 24.04's `python3` package) |
| Framework | FastAPI 0.143.0 (Starlette 1.8.0) |
| Server | Granian 2.8.4 (Rust HTTP server), ASGI interface, uvloop 0.23.0 |
| SQLite driver | the standard library `sqlite3` module, linked to Ubuntu's SQLite 3.45.1 |
| JSON | SQLite's `json_object` renders every response; orjson 3.13.0 only decodes request bodies and JWT parts |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions, transitive ones included, are pinned in `requirements.txt`.

## Running it

```bash
sudo bash install.sh   # apt: python3, python3-venv
bash build.sh          # creates .venv and installs requirements.txt
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` is a single `apt-get` line. `build.sh` and `start.sh` are one-line shims into `infra/ops.py`; all of the logic is Python.

For development there is a [justfile](https://just.systems) (no uv needed, just `python3 -m venv` and pip):

| Command | What it does |
|---|---|
| `just setup` | `.venv` with the first Python ≥ 3.12 on `PATH`, plus py-spy in `.devtools` |
| `just run` | serve on `127.0.0.1:3000` against `build/feed.db` |
| `just test` | unit tests (stdlib `unittest`) |
| `just check` | the official 42-check suite, exactly as CI runs it |
| `just bench local [vus] [duration] [ramp]` | k6 against `just run` |
| `just bench linux <users> [--duration s]`, `just bench ladder` | one run, or the scoring ladder, in Docker with the droplet's limits: the server gets 1 CPU, 1792 MiB with no swap and nofile 65535, built with our own `install.sh` and `build.sh` on Ubuntu 24.04. Load comes from `bench/loadgen.py`, a `load.js` clone that scales past 100k users. Run `just vm start` first (a Colima VM) |
| `just bench compare [rounds]` | head-to-head with `python-fastapi-cknutson12` on this machine: max req/s and server CPU per request |
| `just profile [pyspy\|cprofile] [duration] [vus]` | fresh DB, server, k6 load, then a speedscope flamegraph (`build/profile.json`) or a cProfile dump (`build/profile.prof`, top 25 printed). py-spy needs sudo on macOS |

## Layout

Hexagonal (ports and adapters). `domain/` holds the rules and the five workflows and imports only the
standard library. `adapters/` implements the ports (SQLite store, HS256 verifier, clock, cProfile, descriptor guard)
and the HTTP driving adapter. `infra/` reads settings and runs build/start. `main.py` wires them
together. `bench/` holds the load-testing tools, and `tests/` the unit tests.

## Optimizations, and where they came from

- **SQLite renders the JSON** (from PR #13). `json_object(...)` builds each post and `group_concat`
  builds the feed, so the connection (`text_factory = bytes`) returns finished documents. Python never
  builds a dict or serializes one; handlers only join byte strings.
- **Granian instead of uvicorn** (PR #13): HTTP parsing and connection handling run in Rust. Settings:
  one worker, single-threaded runtime, uvloop, HTTP/1 only, no websockets, no access log, backlog 4096.
  It has to be `asgi` rather than RSGI to keep FastAPI. It isn't `asginl` because the lifespan opens and
  closes the store and the profiler.
- **FastAPI routes are raw ASGI endpoints** (PR #10, which measured −36% CPU per request). There is no
  dependency solving, no `Request`/`Response` objects and no middleware stack
  (`app.middleware_stack = app.router`). Routes are listed in load-mix order (feed, post, like,
  create, health), and the 404 is a constant.
- **FastAPI's native telemetry is off**. Since 0.143, every request checks whether OpenTelemetry is
  configured, and that costs three `os.environ` lookups. Profiling showed it as the biggest Python cost
  after SQLite.
- **Group commit** (PRs #25, #22 and #15). The first write in an event-loop pass runs `BEGIN IMMEDIATE`
  and schedules one `COMMIT` with `loop.call_soon`. Each write's response waits for that commit, so
  nothing is acknowledged before it is durable. Reads that ran while a batch was open wait for it too.
- **One connection, `locking_mode=EXCLUSIVE` before `journal_mode=WAL`** (PRs #25 and #15). The WAL index
  lives in heap memory instead of a `-shm` file. `synchronous=NORMAL`, 1 GiB mmap, 64 MiB page cache.
- **Checkpoints off the commit path** (PR #22). `wal_autocheckpoint=0`, plus a passive checkpoint about
  once a second whenever no batch is open.
- **JWT verified by hand** (PR #1) on every request with `hmac` and `hashlib`: HS256 only, constant-time
  signature compare, and `exp`/`nbf` checked.
- **Likes in one statement**: `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. A
  post-existence check runs only when nothing was inserted.
- **GC** (PR #1): startup objects are frozen and the gen-0 threshold is raised.
- **`--backpressure 65535`**. Granian holds one backpressure permit per open connection, and the default
  is backlog ÷ workers = 4096. In direct mode every user keeps its own connection, so from user 4,097 on,
  connections sat in the kernel queue: at 20k users this gave 22% timeouts with the CPU 7% busy.
- **Descriptor guard** (PR #13). Once a second the server counts `/proc/self/fd`. Above 40,000 open
  descriptors, responses carry `Connection: close` and those users reconnect for their next request.
  This keeps us under `LimitNOFILE=65535` and caps the memory used by idle connections.
- **Keep-alive**: Granian's header-read timeout is 60 s, its maximum. That is far longer than the load
  script's longest pause between requests (15 s).

## Results (Docker, droplet limits, Apple M-series core, 60 s holds)

| Users | Errors | p95 | p99 | Server CPU (run average) | Peak memory |
|---|---|---|---|---|---|
| 20,000 | 0% | 0.3 ms | 3.8 ms | 14% | 594 MiB |
| 40,000 | 0% | 0.8 ms | 9.5 ms | 27% | 872 MiB |
| 80,000 | 0% | 23 ms | 99 ms | 54% | 931 MiB |
| 120,000 | fail | 22 s | 26 s | saturated | 1,745 MiB |

A droplet's vCPU is slower than this core, so expect a lower real limit.

Against the merged FastAPI entry (`just bench compare`, same Mac, same load):

| | python-fastapi-cknutson12 (uvicorn) | this entry (Granian) |
|---|---|---|
| Server CPU per request, saturated | 56 µs | 37 µs (−34%) |
| Server CPU, `load.js` at 2,000 users | 10.4 s | 7.5 s (−29%) |

## License

MIT, under the repo's [license](../../LICENSE).
