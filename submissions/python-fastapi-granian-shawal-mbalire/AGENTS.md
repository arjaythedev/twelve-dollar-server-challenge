# AGENTS.md: python-fastapi-granian-shawal-mbalire

Handoff notes. The source is the source of truth; this file holds the rules, the why, the verified
numbers and what's next. Read it in full before touching code. Last updated 2026-10-16.

## Goal and owner's rules

A $12 Server Challenge entry (`../../README.md`, `../../SPEC.md`): 1 vCPU, 2 GB RAM, score = most
concurrent k6 users held for 5 minutes (p95 < 500 ms, p99 < 1 s, < 1% errors). Open PR: #41.

- **The owner's target: hold 140k–160k users on the bench.** Only the API container is droplet-limited
  (CPU 0, 1792 MiB, no swap, nofile 65535); the owner confirmed these limits and wants no limits adapter.
- **FastAPI served by Granian. Keep this design**, even to chase other entries.
- **SQLite builds the response JSON**; Python only joins byte strings.
- **Hexagonal architecture** (the owner's `hexagonal-architecture` skill): four root folders, IaC in
  `infra/`, logic in named steps and pure rules, ports for anything with side effects.
- **Latest Python**: the owner asked for it; we run python-build-standalone 3.15.0 (see below).
- **justfile** per the `justfile` skill. **No uv** (`python3 -m venv` + pip). **No shell scripts**: logic is
  Python; the required `.sh` files stay one line each.
- **Spell names out** (`descriptor`, not `fd`).
- **Claims must be honest.** Every number in README.md comes from the fixed `just bench linux` and says so.
- Don't commit, push or open PRs until the owner asks. Ask before A/B experiments that change the
  design (`asginl`, `--task-impl rust`, SQL JSON vs orjson, group commit, locking mode, workers).

## Challenge rules (summary of SPEC.md)

SQLite in-process at `$SQLITE_PATH`, schema unchanged. No caching across requests (prepared
statements, page cache and mmap are fine); every JWT verified. Writes durable before the response
(WAL + `synchronous=NORMAL`; group commit OK if each response waits). All 42 checks of `test/test.sh`
byte for byte. `install.sh` / `build.sh` / `start.sh` (env `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`),
healthy within 60 s. We serve **direct** on `0.0.0.0:80`.

## State of the tree

The 2026-10-15/16 sessions are committed locally (see `git log`), **not pushed**. Earlier commits
`03cd105`, `eb832d8`, `d721dea` are on `fork`, branch `python-fastapi-granian-shawal-mbalire`. Tested:
`just test` 50 pass, `just check` 42/42 (CI's `test/run.sh` 42/42 in a clean Ubuntu 24.04 container
with the downloaded Python, 2026-10-15). What the sessions added:

- Python 3.15.0 build (`infra/ops.py`), `ca-certificates` in `install.sh`, `.python/` ignored.
- Dispatcher instead of Starlette routing (`adapters/asgi_http.py`, `tests/test_asgi_http.py`).
- Step chains + `Timed` monad (`domain/models.py`, `domain/railway.py`, `domain/workflows.py`,
  `domain/rules.py`, `domain/ports.py`), span recorder (`adapters/span_recorder.py`), background spans
  in `adapters/sqlite_posts.py`, `PROFILE_HOPS_OUT` (`infra/config.py`, `main.py`), `tests/test_railway.py`.
- Bench: clock keeper, guest-agent stop, idle check, `--repeat`, `--profile layers|python|hops`
  (`bench/linux.py`, `bench/profiling.py`, `infra/iac/bench/*.tf`).
- 2026-10-15/16: descriptor budget counts sockets (sockstat), reserve 4,096, every 50 ms
  (`adapters/descriptor_budget.py`); bench: layered image, cached generator source, sampler + timelines
  (`bench/samples.py`), 120 s / scaled-ramp defaults, ladder `--upper`, `snapshot` / `ab` / `results`
  modes; justfile targets for all of it; README Supported users.
- README.md rewritten with fixed-bench numbers only. **PR #41's description still has the old numbers**;
  sync it when the owner asks to push (`gh pr edit 41 --repo arjaythedev/twelve-dollar-server-challenge --body-file ...`).

## Decisions that are easy to break

- **Granian flags** are in `infra/ops.py` (`GRANIAN_FLAGS`): one worker, `st` runtime, uvloop, `asgi`
  (not `asginl`: the lifespan starts and stops resources). `--backpressure 65535` is critical: the
  default (4096) caps open connections, and every user holds one. `--http1-header-read-timeout` maxes
  out at 60000.
- **Python** comes from python-build-standalone, pinned by release and SHA-256 per architecture in
  `infra/ops.py` (`PYTHON_RELEASE`, `PYTHON_VERSION`, `PYTHON_SHA256`), downloaded into `.python/` on
  Linux only; Mac and `PYTHON=` keep the local interpreter. Its SQLite is 3.53.1 (feed SQL needs ≥ 3.44).
- **No Starlette routing**: `app.middleware_stack` is our `Dispatcher`, an exact path match plus a
  `/posts/` prefix split; only the lifespan reaches `app.router`. Each route is (method, handler,
  render, endpoint name). Wrong method on a known path → 405 JSON (out of scope in SPEC). FastAPI
  telemetry is off (it cost three `os.environ` lookups per request).
- **Workflows are Step chains returning `Timed[Result]`** (Writer monad over spans). `main.py` injects
  `run_untimed` (production, ~0.35 µs/request in-process) or, with `PROFILE_HOPS_OUT`, `TimedRunner`
  + `SpanRecorder` (adds ~2.5–3.3 µs per read). Keep new logic in named steps and `domain/rules.py`.
- **Group commit** (`adapters/sqlite_posts.py`): writes, and reads made during an open batch, await
  the commit. Never answer a write before its commit.
- **`locking_mode=EXCLUSIVE`**, one connection, PASSIVE checkpoint on the event loop every 1 s.
  Dropping EXCLUSIVE (for a checkpoint thread, a read connection or a 2nd worker) cost ~15% CPU (below).
  With EXCLUSIVE the `sqlite3` CLI can't open the DB while the server runs.
- **Descriptor budget** (`adapters/descriptor_budget.py`): while ≥ 61,439 TCP sockets (limit − 4,096)
  are open, per `/proc/self/net/sockstat` (pread every 50 ms, < 1 µs), responses carry `Connection: close`.
  Don't go back to probing one descriptor number: holes below it kept it open for up to 15 s and open
  sockets drained 62k → 8k in waves (+30% reconnects). The probe stays only as the non-Linux fallback. Past ~65k users many
  requests reconnect (54% at 120k with the count; 77–82% with the old probe). The old 256 / 0.25 s ran out between samples: accept
  queue overflowed, clients waited 1 s+ for SYN retries (A/B at 120k: p99 27–175 vs 157–700 ms).

## Layout

```
main.py     composition root
domain/     models (Result, Timed, Span, Step), railway (runners), rules, ports, workflows, errors, constants
adapters/   asgi_http (driving), sqlite_posts, jwt_hs256, clock, span_recorder, descriptor_budget, cprofile_profiler
infra/      config.py, ops.py (build/start; stdlib, Python 3.9), iac/bench/*.tf (bench containers)
tests/      stdlib unittest, fakes for every port (fixtures.py)
bench/      linux.py (the bench), samples.py, profiling.py, journey.py, loadgen.py, idleconns.py, compare.py  ← a fifth root; fold it in some day
```

## Commands

`just test` and `just check` after every server change. `just --list` shows everything. Bench:

- `just vm start` once (Colima profile `bench`, 8 CPUs; it switches the Docker context).
- `just bench linux 40000 --duration 120 [--repeat 3] [--profile [layers|python|hops]] [--submission NAME] [--granian=FLAG] [--no-build]`.
  `NAME` is a reference from `REFS` in `bench/linux.py`: `cknutson12` #1, `tejasnafde` #10,
  `granian-tbsvttr` #13, `go-tbsvttr` #18, `ruby-cicloid` #71, `pypy-pranavds` #84.
  - `--repeat 3`: mean (min–max) of every number, saved under `build/linuxbench/aggregates/`.
  - `--profile layers` (default): `perf` on CPU 0 from a privileged sidecar (`twelve-profiler`, perf
    from the VM kernel's linux-tools), folded to `profiles/<time>/stacks.folded` (opens in speedscope),
    split by binary, kernel cause and thread. Costs ~nothing.
  - `--profile python`: adds `PYTHONPERFSUPPORT=1` for `py::` names; costs ~25% and inflates
    call-heavy Python (routing showed 5.7% vs ~3% timed). Use for relative views only.
  - `--profile hops`: adds the server's own spans (`hops.json`): per endpoint and step, plus
    `background` spans (`group commit`, `wal checkpoint`). Spans are elapsed time, including waits.
- `just bench ladder 130000 --upper 170000 --resolution 10000`: the limit search; `--upper` is a load
  known to fail, so it bisects without the doubling run. Each result has `limited_by`.
- Defaults: 120 s hold (`--duration 300` = the challenge's 5 min), ramp = users / 2000 s clamped to
  20–60 s. Our image is layered (deps before code): a code change rebuilds in ~1 s, not ~100 s.
- Every run records `listen_overflows` (kernel accept-queue drops), `peak_sockets`,
  `seconds_near_descriptor_limit` and `server_log_emfile`, from `twelve-sampler` (reads
  `/proc/<pid>/net/{netstat,sockstat}` 5×/s on CPU 6). Don't list `/proc/<pid>/fd` there: at 65k
  descriptors that made a generator run 0.6 s late.
- **A/B method**: `just bench snapshot a`, change the code, `just bench snapshot b`, then
  `just bench ab 40000 a b [--rounds 3]`: runs alternate (A B A B …) and it prints both sides; saved
  under `build/linuxbench/ab/`. Snapshots are images `twelve-ab-NAME`; after `ab`, `twelve-server-ours`
  is the last variant, so rebuild (plain `just bench linux`) before other runs.
- `just bench results [n]`: last runs as a table. `just bench samples [file] [--bucket s]`: one run's
  timeline (default newest): dropped connections, CPU 0, open/orphan/TIME_WAIT sockets per bucket.
- `just bench journey [name]`: µs per request in-process (the real app, seed DB, real JWT, called
  directly) in the server image or snapshot `name`; for µs-level changes the load bench can't resolve.

## How the bench works (`bench/linux.py`, `infra/iac/bench/`)

- Terraform (kreuzwerker/docker 3.6.2) creates a bridge network, the server (CPU 0, 1792 MiB, no swap,
  nofile 65535, built with our `install.sh`/`build.sh` on Ubuntu 24.04) and 4 generators (CPUs 1-2,
  3-4, 5-6, 7; one address each, ~256k ports). Fresh per run; state in `build/linuxbench/terraform`.
- Load: PR #14's C generator (`load.js` user loop), one process per generator CPU.
- Network work is steered (RPS) so CPU 0 pays for the server's own receive path. **Not loopback**:
  loopback made servers look ~3× cheaper (that's where an old 80k claim came from).
- **The clock must be controlled (found 2026-10-15).** Apple Silicon clocks a core cluster by its load:
  the same server cost 144 µs/request with the VM otherwise idle and 68 with a busy loop elsewhere.
  Colima's `lima-guestagent` scans the VM's sockets for port forwards and spun at 100% under 40k
  connections, which did this by accident, on and off. So `bench/linux.py`:
  - stops the agent for the session (`colima ssh -p bench -- sudo systemctl stop lima-guestagent`,
    restarted in `finally`); if a run is interrupted, start it again by hand;
  - runs `twelve-clock-keeper`, a `chrt -i 0` (SCHED_IDLE) busy loop on CPU 7, for every run. With
    the loop on a CPU of its own (3 generators) the generators fell 37–60 s behind at 140k+;
  - refuses to start unless the VM is < 5% busy, and records `guest_agent_cpu_s` per run.
  The keeper on a shared CPU may cost a little accuracy: one sanity run gave 75 µs vs 63–66 with a
  dedicated keeper CPU, within the day's drift; unresolved.
- **The Mac drifts over hours** (#84: 53 µs in the morning, 65 µs in the afternoon batch, 49–54 when
  alternated with ours). Compare entries only with alternating runs.
- **Metrics**: server µs/request = cgroup CPU ÷ requests (whole run). `core0_util` (hold only, softirq
  included) ÷ requests/s = CPU 0 busy µs per request, kernel included; × a profile share = that layer.
- **The bench isn't a droplet.** An M-series core is several times faster than a droplet vCPU, and a CPU
  cap (`--cpus`) distorts Python more than Go. Estimate droplet scores by CPU per request against
  entries with known droplet scores (#1 11k, #10 22k, #18 22k).

## Verified numbers (fixed bench, 2026-10-15)

40k users, 120 s hold, 0 errors everywhere (README has the full table):

| Entry | server µs/request | `core0_util` | p99 ms |
|---|---|---|---|
| #71 Ruby | 44.5–46.5 | 17% | 3.9–4.7 |
| #84 PyPy (alternated with ours) | 49.4–53.6 | 20–21% | 3.2–4.4 |
| **ours** | **63.4–75.2** (69.7–72.8 when alternated) | 21–25% | 4.7–8.0 |
| #18 Go (droplet 22k) | 80.1–90.0 | 29–31% | 8.7–20 |
| #10 lean FastAPI (droplet 22k) | 98.8–114.9 | 33–37% | 12–1,619 |
| #1 FastAPI + uvicorn (droplet 11k) | 123.4–133.9 | 40–43% | 11–561 |
| #13 Granian RSGI | 112.1–160.4 | 37–54% | 24–62 |

**Supported users (5-minute hold): 120k.** 130k is the edge (passes every 120 s run, 1 of 3 five-minute
holds); 140k fails. 5-minute holds, single runs:

| Users | Build | Pass | `core0_util` | p95 | p99 | `listen_overflows` | Connects | Peak MiB |
|---|---|---|---|---|---|---|---|---|
| 120k | final (socket count) | yes | 70% | 27 ms | 169 ms | 0 | 2.04M | 1,464 |
| 120k | probe | yes | 75% | 31 ms | 300 ms | 0 | 2.91M | 1,373 |
| 130k | final | no | 76% | 48 ms | 1,465 ms | 1,903 | 2.38M | 1,538 |
| 130k | probe | yes | 74% | 18 ms | 84 ms | 0 | 3.22M | 1,311 |
| 130k | probe | no | 85% | 539 ms | 1,212 ms | 612 | 3.26M | 1,348 |
| 140k | final | no (0.08% errors, generator 11 s late) | 81% | 5.1 s | 16.3 s | 578,284 | 2.60M | 1,665 |
| 140k | probe | no | 82% | 202 ms | 4,390 ms | 50,191 | 3.60M | 1,608 |

2-minute runs (ladder, probe build): 130k and 140k pass, 150k fails (86%, 1,267 dropped). They
overstate the limit; confirm with `--duration 300`. The same code varies up to 17% in µs/request
between runs (Mac clock: 58.8 vs 68.9 at 130k), which is why 130k sometimes passes. Timelines at
130–140k: CPU 0 averages 75–85%, a burst hits 95–100%, the accept queue (4096 = somaxconn, can't be
raised under rule 11) overflows, and retries feed the next burst. Memory reaches 1.67 of 1.75 GiB at 140k.

Where CPU 0 goes at 40k (~50 µs busy per request): Python thread ~25 µs (SQLite ~10–12, our hops,
Granian's Python glue, its syscalls), Granian's Rust I/O thread ~15, network interrupts ~10. At
120k+ Granian's threads take ~60% of busy CPU (reconnects) and the kernel ~55%.

Hop spans at 40k (elapsed, timed mode; medians are what a request costs, means include waits):
feed ≤ 33 µs median / 158 mean, post ≤ 33 / 142, like ~260 (always waits for its commit), create
~1 ms (waits ~0.9 ms for Granian to hand over the body). authenticate is 2.6 µs warm but 13–15 µs under
load: the server runs cache-cold with 40k connections, which roughly doubles every Python cost.

## Tried and rejected (with evidence; don't repeat without a new reason)

| Idea | Result |
|---|---|
| Python 3.13/3.14 (deadsnakes, gcc) | ≤ 0.5 µs/request in-process; not worth a PPA |
| JIT (`PYTHON_JIT=1`), `PYTHONMALLOC=mimalloc` | nothing measurable |
| `--task-impl rust` | falls back to asyncio on Python ≥ 3.12 (needs 3.11) |
| `--loop rloop` (0.5.0) | 40k, alternating: 50.5 vs 51.4 µs in the one clean pair; other pairs disturbed. No clear gain, reverted |
| Python 3.11.17 + `--task-impl rust` (same standalone release) | 40k, alternating: 53.4–54.8 vs 55.2–55.6 µs (one 64.6 outlier left out): ~2–3% cheaper, "experimental" per Granian, breaks the latest-Python rule. Not adopted |
| Descriptor reserve 256 / 0.25 s | replaced by 4,096 / 50 ms: accept-queue overflows at 120k+ (see Decisions) |
| HMAC one-shot in the JWT verifier | saves 0.17 µs of a 2.6 µs verify |
| Feed SQL: concat instead of `json_object`, rows + Python join | slower / equal |
| Feed `group_concat` without inner `ORDER BY` | 22 → 17.5 µs, but relies on unspecified order (owner's call) |
| Checkpoint on its own thread + connection (no EXCLUSIVE) | +15% CPU (75.6–83.5 vs 60.8–69.5 µs); stalls remained (commits stalled ≤ 25 ms) |
| Same + `--workers 2` | p99 better (2.6–5.0 vs 4.8–8.4 ms) but CPU 63–86 µs (mean 74): worse at the limit |
| 3 generators + dedicated keeper CPU | generators 37–60 s late at 140k+; reverted to 4 + SCHED_IDLE keeper |

## Next: toward 140k–160k (in order)

1. **Make 130k hold for 5 minutes.** It is CPU headroom now, not a stall bug (timelines above). Cut
   per-connection CPU (accept/close is most of it past 65k users: Granian's threads and the kernel) or
   smooth bursts. Read `just bench samples` for each try; `--profile layers` at 130k shows the split.
2. **Fewer reconnects or cheaper accepts.** ~85% of requests reconnect past 65k users. Granian gives
   Python no handle on idle sockets, so "close the idlest connection" can't be done in the app.
   Options: a smaller per-connection memory footprint does not help (descriptors cap it); Granian
   flags for the accept path are worth reading in its source.
3. **Memory**: 1.6 of 1.75 GiB peak in the 140k hold. Try `MALLOC_ARENA_MAX=2` and a smaller
   `--http1-buffer-size` before going higher.
4. **Per-request CPU** (what decides the droplet score): #84 is ~27% cheaper. Owner's calls: Python 3.11
   + `--task-impl rust`, the feed `ORDER BY` trick (unspecified order; not recommended).
5. Rule 10 risk: python-build-standalone is a prebuilt runtime fetched by build.sh; ask the maintainer
   on PR #41 before relying on it.
6. Two real droplets would turn the estimate into a score (costs money; the owner's call).

## Environment (owner's Mac)

- `python3` is Apple's 3.9.6 (justfile and bench Python must stay 3.9-compatible); Homebrew has 3.14.
- The shell is **nushell** (the Bash tool here runs zsh: `${var%%:*}` works, word-splitting doesn't);
  the justfile sets its own shell.
- macOS numbers mean nothing for capacity (listen backlog 128, 10 cores); use `just bench linux`.
- **Keep the Mac idle during bench runs**: any load there (even `just check`) skews them.
- Two Colima VMs (server and generator apart) don't work: `vz` NAT drops connections past 5–10k.
- Colima profiles `bench-server` and `bench-gen` are stopped leftovers; delete if the owner agrees.
- `~/.docker/config.json` uses `credsStore: desktop`; `bench/linux.py` puts Docker.app's bin on `PATH`.
- `docker run --privileged --pid host` + `nsenter` into the VM is blocked by the permission classifier;
  use `colima ssh -p bench -- ...` for VM-level actions.
