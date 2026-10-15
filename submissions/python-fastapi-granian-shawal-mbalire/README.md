# Python + FastAPI on Granian

| | |
|---|---|
| Language | Python 3.15.0 ([python-build-standalone](https://github.com/astral-sh/python-build-standalone) 20261013: clang, PGO + LTO, tail-call interpreter), pinned by SHA-256 |
| Framework | FastAPI 0.143.0 (Starlette 1.8.0) |
| Server | Granian 2.8.4 (Rust HTTP server), ASGI interface, uvloop 0.23.0 |
| SQLite driver | the standard library `sqlite3` module, with the SQLite 3.53.1 built into that Python |
| JSON | SQLite's `json_object` renders every response; orjson 3.13.0 only decodes request bodies and JWT parts |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions, transitive ones included, are pinned in `requirements.txt`.

## Running it

```bash
sudo bash install.sh   # apt: ca-certificates, python3, python3-venv
bash build.sh          # downloads the pinned Python 3.15, creates .venv, installs requirements.txt
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` is a single `apt-get` line. `build.sh` and `start.sh` are one-line shims into `infra/ops.py`; all of the logic is Python. On Linux, `build` downloads the pinned python-build-standalone 3.15.0 for the machine's architecture, checks its SHA-256 and builds `.venv` from it (stdlib only, no uv). Elsewhere, or with `PYTHON=` set, it uses the local interpreter.

For development there is a [justfile](https://just.systems) (no uv needed, just `python3 -m venv` and pip):

| Command | What it does |
|---|---|
| `just setup` | `.venv` with the first Python ≥ 3.12 on `PATH`, plus py-spy in `.devtools` |
| `just run` | serve on `127.0.0.1:3000` against `build/feed.db` |
| `just test` | unit tests (stdlib `unittest`) |
| `just check` | the official 42-check suite, exactly as CI runs it |
| `just bench local [vus] [duration] [ramp]` | k6 against `just run` |
| `just bench linux <users> [--duration s] [--repeat n] [--profile [layers\|python\|hops]] [--submission name]`, `just bench ladder <start> [--upper n]` | one run (120 s hold by default; `--duration 300` for the challenge's 5 minutes; or `n` runs, as mean and range), or the limit search, in Docker with the droplet's limits (see Results). Terraform (`infra/iac/bench/`) creates the server and four load generators for each run and removes them after. `--profile` samples the server's CPU with `perf` (kernel, Granian, SQLite and Python together); `hops` adds this server's own per-hop timings. `--submission` runs another entry (#1, #10, #13, #18, #71, #84) on the same setup. Needs `terraform`; run `just vm start` first (a Colima VM) |
| `just bench snapshot <name>`, `just bench ab <users> <a> <b> [--rounds 3]` | A/B of two versions: snapshot each working tree into an image, then run them alternately (A B A B …) so the Mac's drift hits both; prints both sides' CPU per request, CPU 0, p95/p99, new and dropped connections. A code change rebuilds in about a second |
| `just bench results [n]`, `just bench samples [file] [--bucket s]` | the last `n` runs as a table; one run's timeline (dropped connections, CPU 0, open sockets) |
| `just bench journey [name]` | µs per request in-process (the app called directly on the seed DB, in the server image or a snapshot): for changes too small for the load bench |
| `just bench compare [rounds]` | head-to-head with `python-fastapi-cknutson12` on this machine: max req/s and server CPU per request |
| `just profile [pyspy\|cprofile] [duration] [vus]` | fresh DB, server, k6 load, then a speedscope flamegraph (`build/profile.json`) or a cProfile dump (`build/profile.prof`, top 25 printed). py-spy needs sudo on macOS |

## Layout

Hexagonal (ports and adapters). `domain/` holds the rules and the five workflows and imports only the
standard library. Each workflow is a chain of named steps (authenticate → decode body → insert post)
run along the `Result` track by a runner; `Timed`, a Writer monad, carries the result plus a span per
step. Production runs them untimed (~0.35 µs per request); with `PROFILE_HOPS_OUT` set, the composition
root swaps in the timed runner and a span recorder, so per-hop profiling measures the real request
path. `adapters/` implements the ports (SQLite store, HS256 verifier, clock, span recorder, cProfile,
descriptor guard) and the HTTP driving adapter. `infra/` reads settings and runs build/start. `main.py` wires them
together. `bench/` holds the load-testing tools, `infra/iac/bench/` the Terraform for the benchmark's
containers, and `tests/` the unit tests.

## Optimizations, and where they came from

- **Python 3.15 from python-build-standalone** instead of Ubuntu's 3.12: about 4% less CPU per
  request on this bench (64.6 vs 67.3 µs, three alternating runs each), all of it in Python and
  SQLite. Its JIT and `PYTHONMALLOC=mimalloc` measured no better and are off.
- **SQLite renders the JSON** (from PR #13). `json_object(...)` builds each post and `group_concat`
  builds the feed, so the connection (`text_factory = bytes`) returns finished documents. Python never
  builds a dict or serializes one; handlers only join byte strings.
- **Granian instead of uvicorn** (PR #13): HTTP parsing and connection handling run in Rust. Settings:
  one worker, single-threaded runtime, uvloop, HTTP/1 only, no websockets, no access log, backlog 4096.
  It has to be `asgi` rather than RSGI to keep FastAPI. It isn't `asginl` because the lifespan opens and
  closes the store and the profiler.
- **FastAPI without its per-request machinery** (PR #10, which measured −36% CPU per request). There
  is no dependency solving, no `Request`/`Response` objects and no middleware stack. Starlette's router
  is skipped too: `app.middleware_stack` is a small dispatcher (an exact path lookup plus a `/posts/`
  split), and only the lifespan goes through `app.router`. In-process, routing a feed/post request
  went from 2.4 to 1.4 µs (about 1% of the request's CPU; too small for `just bench linux` to resolve).
  The 404 and 405 bodies are constants.
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
- **Descriptor guard** (idea from PR #13, budget from PR #71). Every 50 ms the server reads the
  kernel's count of open TCP sockets (`/proc/self/net/sockstat`, one `pread` of a file it keeps open,
  under 1 µs). While 61,439 or more are open (the 65,535 limit minus 4,096), responses carry
  `Connection: close` and those users reconnect for their next request. Two fixes, each measured at
  120–130k users with three alternating runs per side:
  - **The reserve.** Past 65k users 7,000–8,000 connections arrive each second. With 256 spare
    descriptors checked every 0.25 s, the server hit the limit between checks, accepts stalled and
    the kernel dropped connections from its full accept queue (clients wait 1 s or more to retry).
    With 4,096 every 50 ms: p95 11–29 ms against 32–74, p99 27–175 ms against 157–700, no dropped
    connections, same CPU.
  - **Counting instead of probing.** The guard used to check whether descriptor 61,439 was open,
    which is only a count while descriptors fill from the bottom. Once connections close below it,
    that descriptor's idle owner could keep the guard on for up to 15 s, closing every connection
    meanwhile: open sockets drained from 62k to 8k in waves. Counting cut reconnects by 24–30%
    (1.08M against 1.41–1.55M at 130k) at about the same CPU per request.
- **Keep-alive**: Granian's header-read timeout is 60 s, its maximum. That is far longer than the load
  script's longest pause between requests (15 s).

## Results

**Not a droplet score.** These come from `just bench linux` on an Apple M-series machine, whose cores
are faster than a droplet's vCPU. The setup is declared in Terraform (`infra/iac/bench/`): the server
container gets one CPU, 1792 MiB with no swap and nofile 65535, and is built with our own `install.sh`
and `build.sh` on Ubuntu 24.04. Four load-generator containers on the other CPUs run PR #14's C
generator (the `load.js` user loop) over a Docker bridge, not loopback. The kernel's network work for
the server is steered onto the server's CPU, so the server pays for its own TCP stack, as on a real box.

**Every number in an earlier version of this README is withdrawn.** On Apple Silicon the VM's cores run
at a clock set by how busy the whole VM is. Colima's guest agent used to spin at 100% of a CPU during
runs and, on and off, kept the clock high: the same server measured anywhere from 68 to 144 µs per
request. The bench now stops that agent and keeps the clock high on purpose (an idle-priority busy
loop on a load-generator CPU), and refuses to start unless the VM is idle. All numbers below are from
that fixed bench, on 2026-10-15.

### At 40,000 users (60 s ramp, 120 s hold, 0 errors for all)

| Entry | Server CPU per request | CPU 0 busy | p95 / p99 | Peak memory |
|---|---|---|---|---|
| #71 Ruby async | **44.5–46.5 µs** | 17% | 1.4–1.6 / 3.9–4.7 ms | 528–793 MiB |
| #84 PyPy + raw epoll | **49.4–53.6 µs** | 20–21% | 1.6–1.8 / 3.2–4.4 ms | 428–566 MiB |
| **this entry (#41)** | **63.4–75.2 µs** | **21–25%** | **1.9–2.2 / 4.7–8.0 ms** | **953–1,048 MiB** |
| #18 Go net/http (droplet: 22k) | 80.1–90.0 µs | 29–31% | 2.6–3.4 / 8.7–20 ms | 828–1,043 MiB |
| #10 lean FastAPI on uvicorn (droplet: 22k) | 98.8–114.9 µs | 33–37% | 3.3–9.7 / 12–1,619 ms | 733–776 MiB |
| #1 FastAPI on uvicorn (droplet: 11k) | 123.4–133.9 µs | 40–43% | 3.2–3.8 / 11–561 ms | 902–1,048 MiB |
| #13 Granian RSGI | 112.1–160.4 µs | 37–54% | 20–30 / 24–62 ms | 939–1,103 MiB |

Three runs per entry (six for this one). CPU per request is the server's cgroup CPU over the run
divided by requests. The machine drifts a little over hours, so for the closest comparison this entry
and #84 were run alternately, run by run: 69.7 / 72.8 / 71.9 µs against 53.0 / 49.4 / 53.6 µs. **#84
uses about 27% less CPU per request than we do, and #71 about 36% less.** We use 15–35% less than the
two entries that scored 22k on a droplet (#18 and #10), and about 45% less than #1.

### Supported users

**120,000 users hold for the challenge's full 5 minutes** on this bench, with the final code: p95
27 ms, p99 169 ms, 0 errors, no dropped connections, 70% of CPU 0, peak memory 1,464 MiB. An earlier
build (descriptor probe instead of the socket count) also held 120k for 5 minutes (p95 31 ms, p99
300 ms). **130k is the edge:** it passes every 2-minute run but held 5 minutes in only one of three
tries. **140k fails.** The limits are p95 < 500 ms and p99 < 1,000 ms.

5-minute holds (60 s ramp), single runs, 0 errors unless noted:

| Users | Build | Result | CPU 0 busy | p95 | p99 | Dropped connections | New connections | Peak memory |
|---|---|---|---|---|---|---|---|---|
| **120,000** | **final** | **pass** | 70% | 27 ms | 169 ms | 0 | 2.04M | 1,464 MiB |
| 120,000 | probe | pass | 75% | 31 ms | 300 ms | 0 | 2.91M | 1,373 MiB |
| 130,000 | final | fail | 76% | 48 ms | 1,465 ms | 1,903 | 2.38M | 1,538 MiB |
| 130,000 | probe | pass | 74% | 18 ms | 84 ms | 0 | 3.22M | 1,311 MiB |
| 130,000 | probe | fail | 85% | 539 ms | 1,212 ms | 612 | 3.26M | 1,348 MiB |
| 140,000 | final | fail | 81% | 5,138 ms | 16,265 ms | 578,284 | 2.60M | 1,665 MiB (0.08% errors) |
| 140,000 | probe | fail | 82% | 202 ms | 4,390 ms | 50,191 | 3.60M | 1,608 MiB |

Past ~65k users the 65,535-descriptor limit makes many requests open a new TCP connection (54% of requests at
120k with the final code, 77–82% with the old probe), and accepting them is what runs out. The timelines show it: at 130k–140k
CPU 0 averages 75–85%, a burst pushes it to 95–100%, the accept queue fills and the kernel drops
connections, and the retries feed the next burst. The same code also varies by up to 17% in CPU per
request between runs (the Mac's clock), which is why 130k sometimes passes. Treat each row as one sample.

**Expected droplet score: above 22,000 users, not measured.** This entry needs 15–35% less CPU per
request than #18 and #10, which both scored 22k on the droplet. We have not run on a droplet. An
earlier version of this README claimed 80k from a loopback benchmark, which charged most of the
server's network work to the load generator. That claim was wrong.

## License

MIT, under the repo's [license](../../LICENSE).
