# Python + FastAPI on Granian

| | |
|---|---|
| Language | Python 3.11.17 ([python-build-standalone](https://github.com/astral-sh/python-build-standalone) 20261013: clang, PGO + LTO), pinned by SHA-256 |
| Framework | FastAPI 0.143.0 (Starlette 1.8.0) |
| Server | Granian 2.8.4 (Rust HTTP server), ASGI interface, uvloop 0.23.0, Granian's Rust task implementation |
| SQLite driver | the standard library `sqlite3` module, with the SQLite 3.53.1 built into that Python |
| JSON | SQLite's `json_object` renders every response; orjson 3.13.0 only decodes request bodies and JWT parts |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions, transitive ones included, are pinned in `requirements.txt`.

## Running it

```bash
sudo bash install.sh   # apt: ca-certificates, python3, python3-venv
bash build.sh          # downloads the pinned Python 3.11, creates .venv, installs requirements.txt
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` is a single `apt-get` line. `build.sh` and `start.sh` are one-line shims into `infra/ops.py`; all of the logic is Python. On Linux, `build` downloads the pinned python-build-standalone 3.11.17 for the machine's architecture, checks its SHA-256 and builds `.venv` from it (stdlib only, no uv). Elsewhere, or with `PYTHON=` set, it uses the local interpreter.

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
| `just bench entries <users> <name>... [--rounds 3]` | the same alternation for whole entries (`ours` plus the names in `REFS` in `bench/linux.py`), one summary row each |
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

- **Python from python-build-standalone** instead of Ubuntu's 3.12: its 3.15 cost about 4% less CPU
  per request on this bench (64.6 vs 67.3 µs, three alternating runs each), all of it in Python and
  SQLite. Its JIT and `PYTHONMALLOC=mimalloc` measured no better and are off.
- **Python 3.11 with Granian's Rust tasks** (`--task-impl rust`, which Granian calls experimental and
  runs only on 3.11; on 3.12+ it falls back to asyncio tasks): about 7% less CPU per request than 3.15
  at 40k users (49.9 vs 53.6 µs) and 3% at 130k (65.3 vs 67.1 µs), three alternating runs each,
  2026-10-16. The same python-build-standalone release, so the same SQLite 3.53.1.
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

The numbers below were measured on Python 3.15 with asyncio tasks, before the switch to 3.11 with
Rust tasks (3–7% cheaper per request in alternating runs; see Optimizations), except the first row of
the Supported users table, which is the submitted build.

**Every number in an earlier version of this README is withdrawn.** On Apple Silicon the VM's cores run
at a clock set by how busy the whole VM is. Colima's guest agent used to spin at 100% of a CPU during
runs and, on and off, kept the clock high: the same server measured anywhere from 68 to 144 µs per
request. The bench now stops that agent and keeps the clock high on purpose (an idle-priority busy
loop on a load-generator CPU), and refuses to start unless the VM is idle. All numbers below are from
that fixed bench, on 2026-10-15.

### Six entries, same conditions (2026-10-16)

All six entries were run on the same bench in one session with `just bench entries`, which alternates whole
entries run by run (ours, #1, #10, #13, #84, #71, then again) so the Mac's drift hits all of them alike.
Every run: 20 s ramp (users / 2000 s, at least 20 s; 60 s at 130k), 120 s hold. #18 Go was not part of this
session (see below).

**At 40,000 users**, three alternating rounds, 0 errors for all. Range (mean):

| Entry | Server CPU per request | CPU 0 busy | p95 | p99 | Peak memory |
|---|---|---|---|---|---|
| #71 Ruby async | **36.9–39.0 µs (37.9)** | 17–18% | 1.3–1.4 ms | 3.5–4.2 ms | 671–764 MiB |
| #84 PyPy + raw epoll | **39.2–42.6 µs (41.1)** | 20–21% | 1.4–1.7 ms | 3.2–8.4 ms | 513–579 MiB |
| **this entry (#41)** | **59.5–66.2 µs (62.4)** | **24–26%** | **2.1–2.3 ms** | **6.4–10.8 ms** | **992–1,010 MiB** |
| #10 lean FastAPI on uvicorn (droplet: 22k) | 66.4–77.1 µs (71.2) | 25–30% | 1.9–2.8 ms | 4.1–13.5 ms | 767–798 MiB |
| #13 Granian RSGI | 90.7–102.8 µs (97.9) | 36–40% | 17.5–19.5 ms | 19.9–24.4 ms | 950–1,196 MiB |
| #1 FastAPI on uvicorn (droplet: 11k) | 107.6–114.1 µs (110.6) | 41–43% | 2.9–3.1 ms | 6.9–11.4 ms | 990–1,061 MiB |

CPU per request is the server's cgroup CPU over the whole run divided by requests. **#71 uses about 39% less
CPU per request than we do, and #84 about 34% less.** We use about 12% less than #10 (droplet 22k) and 44%
less than #1 (droplet 11k).

**At 130,000 users**, one run each (so one sample per entry, not a limit):

| Entry | Result | Server CPU per request | CPU 0 busy | p95 | p99 | Errors | Peak memory |
|---|---|---|---|---|---|---|---|
| #84 PyPy + raw epoll | **pass** | 50.6 µs | 56% | 8 ms | 15 ms | 0 | 580 MiB |
| #13 Granian RSGI | **pass** | 87.2 µs | 91% | 247 ms | 595 ms | 0 | 1,313 MiB |
| #10 lean FastAPI on uvicorn | fail (errors) | 43.1 µs | 63% | 6 ms | 12 ms | 0.65% | 1,028 MiB |
| #71 Ruby async | fail (p95, p99) | 63.2 µs | 69% | 508 ms | 1,059 ms | 0 | 963 MiB |
| **this entry (#41)** | **fail (p99)** | **77.0 µs** | **82%** | **419 ms** | **1,805 ms** | **0** | **1,477 MiB** |
| #1 FastAPI on uvicorn | fail (CPU, errors) | 69.3 µs | 94% | 1,620 ms | 2,392 ms | 0.63% | 1,353 MiB |

At 130k every entry passes the 65,535-descriptor limit, so CPU per request here is mostly reconnects
and depends on how each server sheds connections (#10's low figure comes with refused connections).

These numbers differ from the ones in the previous version of this README (60 s ramp, entries run in
separate batches): every entry measures 10–20% cheaper now, and #10 about 35% cheaper, so it moved from
well above us to just above us. The code of every entry is pinned to the same commit as before; the
shorter ramp and alternating runs are the changes. The earlier batch measured #18 Go net/http (droplet
22k) at 80.1–90.0 µs per request, 29–31% of CPU 0, when we measured 63.4–75.2 µs; it was not rerun here.

### Supported users

**120,000 users hold for the challenge's full 5 minutes** on this bench, with the submitted build
(Python 3.11 + Rust tasks, 2026-10-16): p95 15 ms, p99 107 ms, 0 errors, no dropped connections, 65% of
CPU 0, 56.0 µs per request, peak memory 1,560 MiB. The 3.15 build held it too (p95 27 ms, p99 169 ms). An earlier
build (descriptor probe instead of the socket count) also held 120k for 5 minutes (p95 31 ms, p99
300 ms). **130k is the edge:** it held 5 minutes in one of three tries, and failed the 2-minute
run in the six-entry comparison above (p99 1.8 s). **140k fails.** The limits are p95 < 500 ms and p99 < 1,000 ms.

5-minute holds (60 s ramp), single runs, 0 errors unless noted:

| Users | Build | Result | CPU 0 busy | p95 | p99 | Dropped connections | New connections | Peak memory |
|---|---|---|---|---|---|---|---|---|
| **120,000** | **submitted (3.11 + Rust tasks)** | **pass** | 65% | 15 ms | 107 ms | 0 | 2.04M | 1,560 MiB |
| 120,000 | final, 3.15 | pass | 70% | 27 ms | 169 ms | 0 | 2.04M | 1,464 MiB |
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

**Expected droplet score: around or above 22,000 users, not measured.** This entry needs about 12%
less CPU per request than #10 and, in an earlier batch, 15–25% less than #18; both scored 22k on the
droplet. We have not run on a droplet. An
earlier version of this README claimed 80k from a loopback benchmark, which charged most of the
server's network work to the load generator. That claim was wrong.

## License

MIT, under the repo's [license](../../LICENSE).
