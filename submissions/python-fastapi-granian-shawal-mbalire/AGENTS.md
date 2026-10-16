# AGENTS.md: python-fastapi-granian-shawal-mbalire

Handoff notes. The source is the source of truth; this file holds the rules, the why, the verified
numbers and what's next. Read it in full before touching code. Last updated 2026-10-16 (submission concluded).

## Goal and owner's rules

A $12 Server Challenge entry (`../../README.md`, `../../SPEC.md`): 1 vCPU, 2 GB RAM, score = most
concurrent k6 users held for 5 minutes (p95 < 500 ms, p99 < 1 s, < 1% errors). Open PR: #41.

- **The owner's target: hold 140k–160k users on the bench.** Only the API container is droplet-limited
  (CPU 0, 1792 MiB, no swap, nofile 65535); the owner confirmed these limits and wants no limits adapter.
- **FastAPI served by Granian. Keep this design**, even to chase other entries.
  Exception: the owner's standard-library HTTP experiment (see "Next step" at the end).
- **SQLite builds the response JSON**; Python only joins byte strings.
- **Hexagonal architecture** (the owner's `hexagonal-architecture` skill): four root folders, IaC in
  `infra/`, logic in named steps and pure rules, ports for anything with side effects.
- **Python 3.11.17 + `--task-impl rust`** (owner's call, 2026-10-16, replacing the earlier latest-Python
  rule): Granian's Rust tasks only run on 3.11 and cost 3–7% less per request than 3.15 (Tried table).
- **justfile** per the `justfile` skill. **No uv** (`python3 -m venv` + pip). **No shell scripts**: logic is
  Python; the required `.sh` files stay one line each.
- **Spell names out** (`descriptor`, not `fd`).
- **Claims must be honest.** Every number in README.md comes from the fixed `just bench linux` and says so.
- Don't commit, push or open PRs until the owner asks. Ask before A/B experiments that change the
  design (`asginl`, task implementation, Python version, SQL JSON vs orjson, group commit, locking mode, workers).

## Challenge rules (summary of SPEC.md)

SQLite in-process at `$SQLITE_PATH`, schema unchanged. No caching across requests (prepared
statements, page cache and mmap are fine); every JWT verified. Writes durable before the response
(WAL + `synchronous=NORMAL`; group commit OK if each response waits). All 42 checks of `test/test.sh`
byte for byte. `install.sh` / `build.sh` / `start.sh` (env `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`),
healthy within 60 s. We serve **direct** on `0.0.0.0:80`.

## State of the tree

All sessions through 2026-10-16 are committed and pushed to `fork`, branch
`python-fastapi-granian-shawal-mbalire` (PR #41, description synced 2026-10-16). Tested: `just test` 50 pass,
`just check` 42/42, CI's `test/run.sh` 42/42 in a clean Ubuntu 24.04 container with the downloaded
Python 3.11.17 (2026-10-16). Submitted build: Python 3.11.17 + `--task-impl rust`, `FeedApp`; it held
120k for 5 minutes (p95 15 ms, p99 107 ms, 65% CPU 0, 56.0 µs/request, peak 1,560 MiB, 2026-10-16 18:09).
What the sessions added:

- Python build from python-build-standalone (`infra/ops.py`; 3.15.0 at first, 3.11.17 + Rust tasks
  since 2026-10-16), `ca-certificates` in `install.sh`, `.python/` ignored.
- Dispatcher instead of Starlette routing, `FeedApp` (`adapters/asgi_http.py`, `tests/test_asgi_http.py`).
- Step chains + `Timed` monad (`domain/models.py`, `domain/railway.py`, `domain/workflows.py`,
  `domain/rules.py`, `domain/ports.py`), span recorder (`adapters/span_recorder.py`), background spans
  in `adapters/sqlite_posts.py`, `PROFILE_HOPS_OUT` (`infra/config.py`, `main.py`), `tests/test_railway.py`.
- Bench: clock keeper, guest-agent stop, idle check, `--repeat`, `--profile layers|python|hops`
  (`bench/linux.py`, `bench/profiling.py`, `infra/iac/bench/*.tf`).
- Descriptor budget counts sockets (sockstat), reserve 4,096, every 50 ms
  (`adapters/descriptor_budget.py`); bench: layered image, cached generator source, sampler + timelines
  (`bench/samples.py`), 120 s / scaled-ramp defaults, ladder `--upper`, `snapshot` / `ab` / `entries` / `results`
  modes, saved server logs and memory timelines; justfile targets for all of it.
- README.md with fixed-bench numbers only. The entries comparison in it (and in Verified numbers) is
  from the 3.15 build; rerun `just bench entries` before quoting a gap to #84 for the current build.

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
  under `build/linuxbench/ab/`.
- **Entries**: `just bench entries 40000 ours cknutson12 tejasnafde ... [--rounds 3] [--no-build]`
  alternates whole entries the same way and prints one row each; saved under `build/linuxbench/entries/`. Snapshots are images `twelve-ab-NAME`; after `ab`, `twelve-server-ours`
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

## Verified numbers (fixed bench, 2026-10-15/16)

Six entries, same session, `just bench entries` (alternating; 20 s ramp at 40k, 60 s at 130k; 120 s
hold), 2026-10-16. Logs in `build/linuxbench/entries/`. 40k, 3 rounds, 0 errors, range (mean):

| Entry | server µs/request | `core0_util` | p99 ms |
|---|---|---|---|
| #71 Ruby | 36.9–39.0 (37.9) | 17–18% | 3.5–4.2 |
| #84 PyPy | 39.2–42.6 (41.1) | 20–21% | 3.2–8.4 |
| **ours** | **59.5–66.2 (62.4)** | 24–26% | 6.4–10.8 |
| #10 lean FastAPI (droplet 22k) | 66.4–77.1 (71.2) | 25–30% | 4.1–13.5 |
| #13 Granian RSGI | 90.7–102.8 (97.9) | 36–40% | 19.9–24.4 |
| #1 FastAPI + uvicorn (droplet 11k) | 107.6–114.1 (110.6) | 41–43% | 6.9–11.4 |

130k, 1 run each: pass #84 (50.6 µs, 56%, p99 15 ms) and #13 (87.2, 91%, p99 595); fail #10 (0.65%
errors), #71 (p99 1,059), **ours (77.0 µs, 82%, p95 419, p99 1,805, 0 dropped)**, #1 (94%, 0.63% errors).
130k, ours vs #13 alternating, 3 rounds, 120 s hold (2026-10-16 05:42–06:01, log
`build/linuxbench/entries/20261016-060136.json`): both 3/3 pass. Ours 61.5–63.1 µs (CPU 0 66–69%,
p99 23–28 ms, peak MiB 1,412–1,732); #13 79.7–82.1 µs (85–88%, p99 36–120 ms). None of ours hit the
pause seen earlier that day (2 of 4 runs failed at 75–80% CPU), and those earlier runs measured 77 µs, so
the early-morning Mac was ~20% faster: the pause seems to need CPU 0 near saturation. Those runs had
a stall logger (below): **every PASSIVE checkpoint blocks the event loop 13–37 ms (mean ~20 ms), once a
second**; one took 72.5 ms and matched the only loop lag > 50 ms; no group commit took > 10 ms. The
fsync is in the VM's virtio disk, so a droplet may differ.
Versus the 2026-10-15 batch (60 s ramp, separate batches) every entry is 10–20% cheaper and #10 ~35%
cheaper (was 98.8–114.9): our margin over #10 shrank from ~35% to ~12%. #18 Go (droplet 22k) was only in
the old batch: 80.1–90.0 µs when ours was 63.4–75.2.

**Supported users (5-minute hold): 120k.** 130k is the edge (1 of 3 five-minute holds; failed the
2026-10-16 120 s entries run); 140k fails. 5-minute holds, single runs:

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
| Python 3.11.17 + `--task-impl rust` (same standalone release) | 40k, alternating: 53.4–54.8 vs 55.2–55.6 µs (one 64.6 outlier left out): ~2–3% cheaper, "experimental" per Granian; not adopted then (latest-Python rule). Re-run 2026-10-16 (3 rounds each, alternating): 40k 47.9–53.4 (49.9) vs 50.8–57.0 (53.6) µs, ~7%; 130k 63.3–66.8 (65.3) vs 65.7–68.2 (67.1), ~3%, both 3/3 pass, p99 170 vs 228 ms mean. 42/42 on 3.11.17 (SQLite 3.53.1, same release; SHA-256 aarch64 `643954c0…ac24`, x86_64 `846bf682…644c`). **Adopted by the owner, 2026-10-16** (in `infra/ops.py`); the Verified numbers above were measured on 3.15 |
| Descriptor reserve 256 / 0.25 s | replaced by 4,096 / 50 ms: accept-queue overflows at 120k+ (see Decisions) |
| HMAC one-shot in the JWT verifier | saves 0.17 µs of a 2.6 µs verify |
| Feed SQL: concat instead of `json_object`, rows + Python join | slower / equal |
| Feed `group_concat` without inner `ORDER BY` | 22 → 17.5 µs, but relies on unspecified order (owner's call) |
| Checkpoint on its own thread + connection (no EXCLUSIVE) | +15% CPU (75.6–83.5 vs 60.8–69.5 µs); stalls remained (commits stalled ≤ 25 ms) |
| Same + `--workers 2` | p99 better (2.6–5.0 vs 4.8–8.4 ms) but CPU 63–86 µs (mean 74): worse at the limit |
| 3 generators + dedicated keeper CPU | generators 37–60 s late at 140k+; reverted to 4 + SCHED_IDLE keeper |
| Descriptor budget at 40,000 open (as #13 does) instead of 61,439 | 130k A/B, alternating, 2026-10-16: 0 of 2 passes (p99 2.0 and 5.3 s). Closing earlier only adds reconnects |
| TCP_DEFER_ACCEPT (as #84) on Granian's listening socket, set from the lifespan | 130k A/B, 3 rounds: 60.0 vs 59.5 µs, same connects (1.077M), no overflows either way; one p99 of 357 ms. Granian's accept path doesn't gain. Note: Granian's worker gets the listening socket only after the lifespan starts |
| Uvicorn 0.54 (uvloop, httptools, #10's settings) instead of Granian, same app (an `HTTP_SERVER=uvicorn` switch in `infra/ops.py`, removed again) | 40k A/B, 2026-10-16. Clean first pair 51.2 vs 45.9 µs (Granian 45.3 in round 2), so uvicorn costs ~12% more per request; ~200 MiB less memory. Later rounds were 35 min apart with p99 300–480 ms (Mac disturbed), same direction: 53.7–55.4 vs 54.1. Not run at 130k |
| Granian on PyPy 3.11 (7.3.23; build path removed from the tree on the owner's call, 2026-10-16) | 42/42 and 50 tests pass, but **OOM-killed in every 40k round**: the worker grows ~6 KB per request, outside PyPy's GC heap (`PYPY_GC_MAX=400MB` was passed without a MemoryError; `gc.collect()` every 0.5–1 s reclaims nothing and takes ~70 ms). Bisected in a container with a bare ASGI app: leaks with Granian's asyncio tasks always; with `--task-impl rust` only once the live heap is bigger (importing FastAPI, or 40 MiB of plain objects, triggers it). Same in Granian 2.7.3, 2.8.0, 2.8.4: a Granian/PyO3-on-PyPy bug. Also: PyPy 8.0 has wheel ABI pp80 (no Granian wheel); rloop's PyPy wheel fails to import (`_PyBytes_Resize`); in-process journey 15.3 vs 13.3 µs (CPython). Repro: a 10-line ASGI app that answers `{"ok":true}`, plus `BALLAST = [str(i) * 3 for i in range(800000)]` at import, under `granian --interface asgi --loop asyncio --task-impl rust`; one keep-alive `http.client` loop; worker VmRSS before/after 30k requests |
| Constant response-start messages without our `content-length` (Granian adds it) + `FeedApp` (below) | 40k A/B, 4 rounds, 2026-10-16: 51.6–64.8 (57.8) vs 52.8–57.5 (54.6) µs, worse in 3 of 4 pairs, p99 5.4 vs ~3.7 ms. Granian's path without a content-length header is slower; `_send` reverted. `FeedApp` (FastAPI subclass sending HTTP straight to the Dispatcher; profile: FastAPI+Starlette `__call__` ≈ 1 µs/request) kept: measured on its own below |
| `FeedApp` alone, **kept** (`adapters/asgi_http.py`: HTTP goes straight to the Dispatcher, skipping FastAPI's and Starlette's `__call__`) | 40k A/B, 8 rounds, 2026-10-16 14:43–15:24, 3.11 + Rust tasks (`build/linuxbench/ab/20261016-152408.json`): 50.8–62.7 (55.2) vs plain `FastAPI` 53.2–64.6 (58.4) µs, cheaper in 7 of 8 pairs, p99 3.7–6.4 vs 3.6–273.6 ms (one plain-FastAPI run had a late generator). More than the ~1 µs the profile predicted; read it as ≥ 2% |
| `MALLOC_ARENA_MAX=2` (set in `start()` before exec) | 40k A/B, 4 rounds, 2026-10-16 (`build/linuxbench/ab/20261016-154626.json`): 50.8–54.2 (53.2) vs default 50.8–60.3 (53.5) µs, peak 986 vs 988 MiB, steady 624–627 MiB both. No effect on CPU or memory; not adopted. Not run at 130k, where memory is tight |
| `--http1-buffer-size 8192` (default 417,792) | 40k A/B, 2 rounds, 2026-10-16 (`build/linuxbench/ab/20261016-161347.json`): steady memory 624–625 vs 624–626 MiB, 48.3–63.9 vs 53.2–60.9 µs. It caps the read buffer's growth; it isn't the initial allocation, so it saves nothing. Not adopted |
| Checkpoint every 5 s instead of 1 s (fewer ~20 ms stalls) | 130k A/B, 3 rounds, 2026-10-16 (`build/linuxbench/ab/20261016-163410.json`): 2 of 3 passes vs 3 of 3; p99 49.8–2,141 (881) vs 27.6–765 (329) ms; peak 1,528–1,586 vs 1,399–1,460 MiB (the bigger WAL is charged to the cgroup); µs/request equal (67.1 vs 66.1). Each stall grows with the WAL, so fewer, longer stalls hurt more. Not adopted |
| Python GC as the 130k pause | every GC pass logged in a 130k run: none > 5 ms, no full collection. That run passed, so it rules GC out only for a run without the pause |

## Next step (owner, 2026-10-16): a standard-library HTTP server, no FastAPI, no Granian

**Status (2026-10-17): underway in `../python-stdlib-shawal-mbalire/`; its AGENTS.md has the state,
numbers and next steps** (epoll server adopted; 180k passed a 2-minute ladder run). The `bench/linux.py`
changes for it (REFS `stdlib`, `STDLIB_DOCKERFILE`, REFS `env`) are committed here (2026-10-17).

PR #41 (FastAPI + Granian) is concluded and pushed; leave its design alone. The owner's next step is
an experiment: serve the same five endpoints with the **Python standard library only**, to see how
close plain CPython gets to #84 (PyPy + raw epoll, ~34% cheaper) and #71. This lifts the "FastAPI
served by Granian" rule **for the experiment only**; every other rule above still holds (SQLite builds
the JSON, hexagonal, justfile, no uv, no shell scripts, spelled-out names, honest claims, ask before
committing or pushing, Mac idle during bench runs).

**Decisions** (all made by the owner):
- **Decided (owner, 2026-10-16): a new submission folder** with its own branch and PR, e.g.
  `submissions/python-stdlib-shawal-mbalire/`. Copy what it reuses (domain, store, JWT, descriptor
  budget, tests, justfile, infra) from this folder; don't import across folders. The bench lives here:
  add the new folder to `REFS` in `bench/linux.py` so `just bench entries` can alternate it with `ours`.
- **Decided (owner, 2026-10-16): the latest Python and its features.** Pin python-build-standalone
  3.15.0, release 20261013 (SHA-256s in commit `5111391`'s `infra/ops.py`: aarch64 `26a54e8f…bcd7`,
  x86_64 `e02a368d…8921`; check for a newer release first). Write modern Python (PEP 695 type
  parameters and `type` aliases, `match`, dataclasses with slots). Re-measure the runtime features
  here, since the earlier "no gain" was under Granian: the JIT (`PYTHON_JIT=1`), `PYTHONMALLOC=mimalloc`,
  and the tail-call interpreter (already in this build). The free-threaded build doesn't help on one vCPU.
- **Decided (owner, 2026-10-16): no uvloop; the standard library as much as possible; hexagonal
  architecture** (the owner's `hexagonal-architecture` skill, as in this folder). So: stdlib `asyncio`
  event loop, stdlib `json` instead of orjson (JWT parts, request bodies; SQLite still renders every
  response), stdlib `sqlite3`, `hmac`/`hashlib`. `requirements.txt` should end up empty or gone; any
  third-party package needs a measured reason and the owner's OK.

**What "standard http lib" can mean, and what to expect:**
1. `http.server.ThreadingHTTPServer`: one thread per connection. Every user holds a keep-alive socket,
   so 40k users = 40k threads (stacks, GIL hand-offs); it will not reach 40k in 1.75 GiB. Worth one
   small run (e.g. 2k–5k users) as a baseline only, and it parses headers with `email.parser` (slow).
2. **`asyncio` + `asyncio.Protocol`** with a hand-written HTTP/1.1 parser: the realistic option. One
   thread, `data_received` splits the request line and headers with `bytes.find`/`split`, answers with
   `transport.write(prebuilt header + body)`. This is roughly #84's design on CPython. Study #84
   (`pypy-pranavds` in `bench/linux.py` `REFS`) and #71 for their parsers, keep-alive and close
   handling, and TCP_DEFER_ACCEPT on the listener (no gain under Granian, may help here).
3. `selectors`/raw `epoll` loop by hand: the fastest stdlib path, more code; try only if 2 is close.

**How to build it here (hexagonal):** the domain, workflows, `adapters/sqlite_posts.py`,
`adapters/jwt_hs256.py`, `adapters/descriptor_budget.py` and the clock are framework-free; reuse them.
Only the driving adapter changes: a new `adapters/stdlib_http.py` (protocol + parser) with the
same `Handlers`/route table and renderers as `adapters/asgi_http.py`. The lifespan becomes plain `start()`/`close()` calls around
`loop.create_server(...)`; `main.py` picks the adapter. Group commit already runs on the event loop
and needs no change. Things Granian did for us that the new adapter must do itself:
- HTTP/1.1 keep-alive, `Content-Length` bodies (POST /posts), pipelined requests in one read, 400 on
  garbage, `Connection: close` when the descriptor budget is strained (then close after the write).
- Idle connections stay open ≥ 65 s (SPEC); backlog 4096 (`create_server(backlog=4096)`).
- Descriptor limit 65535: accept errors (EMFILE) must not kill the server; asyncio logs and retries.
- All 42 checks byte for byte (`just check`), plus unit tests for the parser with fakes.

**Measure it the usual way:** `just test`, `just check`, then `just bench entries 40000 ours stdlib pypy-pranavds
ruby-cicloid --rounds 3` (with the new folder in `REFS`)
for the gap, then the 130k/ladder runs. Record every result in this file's tables.

## Still open for the FastAPI + Granian build (PR #41)

1. **130k for 5 minutes**: CPU headroom, not a stall bug. Past 65k users ~85% of requests reconnect,
   and accepting them (Granian's threads + kernel) is most of the CPU; Granian gives Python no handle on
   idle sockets. Checkpoint stalls (~20 ms each second, two fsyncs) remain; every 5 s was worse;
   every 0.5 s is untested (measure stall sizes with `--profile hops` first).
2. **Memory**: 1.56 GiB peak at 120k. `MALLOC_ARENA_MAX=2` and a smaller `--http1-buffer-size` did
   nothing (~15 KB per open connection, steady).
3. **Per-request CPU**: rerun `just bench entries` on the submitted build before quoting a gap to #84
   (the table is from the 3.15 build). The feed `ORDER BY` trick is the owner's call (not recommended).
4. **Maintainer**: PR #41's description asks whether a prebuilt python-build-standalone runtime is OK
   (rule 10). Watch for the answer; CI on the fork needs a maintainer to approve the workflow run.
5. Two real droplets would turn the estimate into a score (costs money; the owner's call).

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
