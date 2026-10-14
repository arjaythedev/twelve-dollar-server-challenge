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
| `just bench linux <users> [--duration s] [--submission name]`, `just bench ladder` | one run, or the scoring ladder, in Docker with the droplet's limits (see Results). Terraform (`infra/iac/bench/`) creates the server and four load generators for each run and removes them after. `--submission` runs another entry (#1, #10, #13, #18, #71, #84) on the same setup. Needs `terraform`; run `just vm start` first (a Colima VM) |
| `just bench compare [rounds]` | head-to-head with `python-fastapi-cknutson12` on this machine: max req/s and server CPU per request |
| `just profile [pyspy\|cprofile] [duration] [vus]` | fresh DB, server, k6 load, then a speedscope flamegraph (`build/profile.json`) or a cProfile dump (`build/profile.prof`, top 25 printed). py-spy needs sudo on macOS |

## Layout

Hexagonal (ports and adapters). `domain/` holds the rules and the five workflows and imports only the
standard library. `adapters/` implements the ports (SQLite store, HS256 verifier, clock, cProfile, descriptor guard)
and the HTTP driving adapter. `infra/` reads settings and runs build/start. `main.py` wires them
together. `bench/` holds the load-testing tools, `infra/iac/bench/` the Terraform for the benchmark's
containers, and `tests/` the unit tests.

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
- **Descriptor guard** (PR #13, budget from PR #71). Every 0.25 s one `fstat` checks whether
  descriptor 65,279 (the limit minus 256) is open. Linux hands out the lowest free descriptor, so it
  is open only once that many are in use. While it is, responses carry `Connection: close` and
  those users reconnect for their next request. This keeps us under `LimitNOFILE=65535` without
  retiring connections early.
- **Keep-alive**: Granian's header-read timeout is 60 s, its maximum. That is far longer than the load
  script's longest pause between requests (15 s).

## Results

**Not a droplet score.** These come from `just bench linux` on an Apple M-series machine, whose cores
are faster than a droplet's vCPU. The setup is declared in Terraform (`infra/iac/bench/`): the server
container gets one CPU, 1792 MiB with no swap and nofile 65535, and is built with our own `install.sh`
and `build.sh` on Ubuntu 24.04. Four load-generator containers on the other CPUs run PR #14's C
generator (the `load.js` user loop) over a Docker bridge, not loopback. The kernel's network work for
the server is steered onto the server's CPU, so the server pays for its own TCP stack, as on a real box.

### Every Python entry at 40,000 users (60 s ramp, 120 s hold, 0 errors for all)

| Entry | Server CPU per request | CPU 0 busy | p95 / p99 | Peak memory |
|---|---|---|---|---|
| #84 PyPy + raw epoll | **50 µs** | 21% | 1.0 / 3.5 ms | 404 MiB |
| **this entry (#41)** | **73–81 µs** | 27% | 2.0–2.3 / 4.0–6.8 ms | 900–980 MiB |
| #10 lean FastAPI on uvicorn | 81 µs | 27% | 1.9 / 6.1 ms | 779 MiB |
| #13 Granian RSGI | 120 µs | 39% | 18.9 / 22.3 ms | 926 MiB |
| #1 FastAPI on uvicorn | 130 µs | 41% | 2.7 / 7.3 ms | — |

Ranges are repeat runs. CPU per request is the server's cgroup CPU over the run divided by requests.
This entry ties #10 and uses about 40% less CPU per request than #1 and #13. #84, which uses no
framework and runs on PyPy, uses about a third less than we do.

### Against the two PRs that benchmarked this one (#84 and #71)

PR #84 (Python on PyPy, raw epoll) and PR #71 (Ruby, async) both compared themselves with this entry
on their own machines. Here are their current versions and ours on the same bench, at the same loads:

| Users | Entry | Server CPU per request | CPU 0 busy | p99 | Peak memory | Result |
|---|---|---|---|---|---|---|
| 40,000 | #84 PyPy + epoll | 50 µs | 21% | 3.5 ms | 404 MiB | pass |
| | #71 Ruby async | 50 µs | 21% | 4.4 ms | 525 MiB | pass |
| | **this entry** | **73–81 µs** | **27%** | **4.0–6.8 ms** | **900–980 MiB** | **pass** |
| 80,000 | #84 | 51 µs | 38% | 8.9 ms | 617 MiB | pass |
| | #71 | 54 µs | 38% | 13.8 ms | 602 MiB | pass |
| | **this entry** | **74–75 µs** | **50%** | **15–16 ms** | **1,411–1,422 MiB** | **pass** |
| 140,000 | #84 | 51 µs | 61% | 17 ms | 460 MiB | pass |
| | #71 | 69 µs | 79% | 407 ms | 744 MiB | pass |
| | **this entry** | **73–79 µs** | **84–87%** | **433–1,758 ms** | **1,283–1,474 MiB** | **one pass, one fail** |

Both use about a third less CPU per request than this entry, and less memory. #84's lead grows with
load: past the 65,535-descriptor limit every entry turns most requests into new connections, and #84
reconnects most cheaply. #84's own README claims 160k users. That was measured over loopback,
which hides most of the network cost, so this bench can't confirm it, and neither can a droplet
until one is run.

What each PR measured for this entry on its own setup, and why it differs from the table above:

| | Their setup | Their numbers for this entry | Their numbers for themselves |
|---|---|---|---|
| #84 | One 4-vCPU Xeon VM, server pinned to one core, load over loopback, 20k-descriptor limit, 15 s ramp and 45 s hold | 40k users pass, 60k fail | 140k pass (160k with a 5-minute hold) |
| #71 | Two VMs (server and generator apart), same C generator | 25k: 47% CPU, p99 26 ms. 40k: 70% CPU, p99 1,660 ms | 25k: 27% CPU. 40k: 40% CPU, p99 28 ms |

- **#84's 20k-descriptor limit** made our old code retire connections at about 19k, so from 40k on most of our requests were new connections, which is where Granian is expensive. The version measured here keeps connections open until the real 65,535 limit.
- **#71's 40k tail** didn't reproduce here (p99 4.0–6.8 ms). Their README attributes part of it to their load generator falling behind. Our earlier code also retired connections at 40k and stalled its event loop for 16 ms every second; both are fixed in this version.
- **The ranking is the same everywhere:** both entries need less CPU per request than this one.

### Where this entry gives out

`just bench ladder 160000 --duration 120 --resolution 10000`: the highest passing load here is
**130,000 users** (p99 203 ms). **CPU fails first**: 83% busy at 130k, 98% at 160k. Past ~65k
users nearly every request is a new TCP connection, and the kernel's share of the core grows to a
third. Memory comes second (1.5 of 1.75 GiB at 160k). There were no errors at any load.

**Expected droplet score: about 22,000 users.** #10 costs the same per request and scored 22k on the
droplet. We have not run on a droplet ourselves. An earlier version of this README claimed 80k from a
loopback benchmark, which charged most of the server's network work to the load generator. That claim
was wrong.

## License

MIT, under the repo's [license](../../LICENSE).
