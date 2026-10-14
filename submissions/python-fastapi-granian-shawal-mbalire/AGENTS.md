# AGENTS.md: python-fastapi-granian-shawal-mbalire

Handoff notes. The source is the source of truth; this file holds the rules, the why, and what's next.
Read it in full before touching code.

## Goal and owner's rules

A $12 Server Challenge entry (`../../README.md`, `../../SPEC.md`): 1 vCPU, 2 GB RAM, score = most
concurrent k6 users held for 5 minutes. Open PR: #41.

- **FastAPI served by Granian. Keep this design**, even to chase other entries.
- **SQLite builds the response JSON**; Python only joins byte strings.
- **Hexagonal architecture** (the owner's `hexagonal-architecture` skill): four root folders, IaC in `infra/`.
- **justfile** per the `justfile` skill. **No uv** (`python3 -m venv` + pip). **No shell scripts**: logic is
  Python; the required `.sh` files stay one line each.
- **Spell names out** (`descriptor`, not `fd`).
- **Claims must be honest.** Every number in README.md comes from `just bench linux` and says so.
- Don't commit, push or open PRs until the owner asks. Ask before A/B experiments that change the
  design (`asginl`, `--task-impl rust`, SQL JSON vs orjson, group commit on/off).

## Challenge rules (summary of SPEC.md)

SQLite in-process at `$SQLITE_PATH`, schema unchanged. No caching across requests (prepared
statements, page cache and mmap are fine); every JWT verified. Writes durable before the response
(WAL + `synchronous=NORMAL`; group commit OK if each response waits). All 42 checks of `test/test.sh`
byte for byte. `install.sh` / `build.sh` / `start.sh` (env `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`),
healthy within 60 s. We serve **direct** on `0.0.0.0:80`.

## Decisions that are easy to break

- **Granian flags** are in `infra/ops.py` (`GRANIAN_FLAGS`). `--backpressure 65535` is critical: the
  default (4096) caps open connections, and every user holds one. Interface `asgi`, not `asginl`,
  because the lifespan starts and stops resources. `--http1-header-read-timeout` maxes out at 60000.
- **Routes are raw ASGI endpoints** (`app.middleware_stack = app.router`): no dependencies, `Request`
  objects or middleware. FastAPI telemetry is off (it cost three `os.environ` lookups per request).
- **Group commit** (`adapters/sqlite_posts.py`): writes, and reads made during an open batch, await
  the commit. Never answer a write before its commit.
- **Descriptor budget** (`adapters/descriptor_budget.py`): while descriptor 65,279 (limit − 256) is
  open, responses carry `Connection: close`. One `os.fstat` every 0.25 s. Past ~65k users every
  request beyond the budget is a reconnect.

## Layout

```
main.py     composition root        domain/   rules, ports, models, workflows (stdlib only)
adapters/   sqlite_posts, jwt_hs256, clock, asgi_http (driving), descriptor_budget, cprofile_profiler
infra/      config.py, ops.py (build/start; stdlib, Python 3.9), iac/bench/*.tf (bench containers)
tests/      stdlib unittest, fakes for every port
bench/      linux.py (the bench), loadgen.py, idleconns.py, compare.py  ← a fifth root; fold it in some day
```

## Commands

`just test` (28 unit tests) and `just check` (the 42-check CI gate) after every server change.
`just --list` shows everything; the bench commands:

- `just vm start` once (Colima profile `bench`, 8 CPUs; it switches the Docker context).
- `just bench linux 40000 --duration 120 [--submission NAME] [--granian=FLAG]`: one run. `NAME` is a
  reference entry from `REFS` in `bench/linux.py` (#1, #10, #13, #18, #84).
- `just bench ladder 160000 --duration 120 --resolution 10000`: the limit search, starting at the
  given user count. Each result has `limited_by`: what gave out first.
- `just bench memory --steps 20000,40000`: memory per idle connection.
- `just profile cprofile`: cProfile on the Mac (not representative of Linux; see next steps).

## How the bench works (`bench/linux.py`, `infra/iac/bench/`)

- Terraform (kreuzwerker/docker 3.6.2) creates a bridge network, the server (CPU 0, 1792 MiB, no swap,
  nofile 65535, built with our `install.sh`/`build.sh` on Ubuntu 24.04) and 4 generators (CPUs 1-2,
  3-4, 5-6, 7; one address each, so ~256k ports). Fresh per run; state in `build/linuxbench/terraform`.
- Load: PR #14's C generator (`load.js` user loop), one process per generator CPU.
- Network work is steered (RPS) so the server's CPU 0 pays for its own receive path. **Not loopback**:
  loopback (our old bench, PR #84's harness) put the server's TCP work on the generator's CPUs and
  made servers look ~3× cheaper. That is where the old 80k README claim came from.
- **Compare by server CPU µs per request** (`server_cpu_s ÷ requests`, same users and duration, two
  runs each). `core0_util` (hold only, softirq included) is noisier. `results.jsonl` in
  `build/linuxbench/` keeps every run.
- **The bench isn't a droplet.** An M-series core is several times faster than a droplet vCPU, and a CPU
  cap (`--cpus`) doesn't model that (it hurt Python far more than Go). Estimate droplet scores against
  reference entries with known droplet scores, by CPU per request.

## Handoff (2026-10-15)

- **Git:** everything is committed and pushed to `fork` (`shawal-mbalire/twelve-dollar-server-challenge`),
  branch `python-fastapi-granian-shawal-mbalire`; latest commits `d721dea` (descriptor budget,
  Terraform bench, honest results) and `eb832d8` (README: #84 and #71). PR #41 updates from it.
- **PR #41's description** was rewritten to match README "Results". Keep the two in sync whenever
  the numbers change (`gh pr edit 41 --repo arjaythedev/twelve-dollar-server-challenge --body-file ...`).
- **Waiting on the owner:** (1) build `--profile` for `just bench linux` (step 2 below; offered, not
  started); (2) a 5-minute hold at 80k to rule out memory growth (all high-load runs held 120 s);
  (3) deleting the stopped Colima profiles `bench-server` and `bench-gen`.
- **The owner's last question was why we fail p99 at 140k.** Measured: CPU 0 is 84–87% busy, the
  kernel takes 28% of it for ~1.5M reconnects, and one-core queueing makes p99 explode past ~80%
  (121 ms at 76%, 203 ms at 83%, 1.8 s at 87%). Inferred, not yet profiled: our per-request cost is
  the Granian → ASGI → Starlette → handler stack (scope dict, awaitables, a task per request, route
  matching), plus stalls from GC, the 1 s WAL checkpoint and group commits that show up in p99 on a
  full core. The profile in step 2 should confirm or correct this.

## Where we stand (2026-10-15)

Every Python entry, plus #71 (Ruby), at 40k users, 120 s hold, 0 errors:

| Entry | µs CPU / request | `core0_util` | p99 ms | Peak MiB |
|---|---|---|---|---|
| #84 PyPy + raw epoll | **50** | 21% | 3.5 | 404 |
| #71 Ruby async (not Python; benchmarked us) | **50** | 21% | 4.4 | 525 |
| **ours (#41)** | **73–81** | 27% | 4.0–6.8 | 900–980 |
| #10 lean FastAPI (droplet: 22k) | 81 | 27% | 6.1 | 779 |
| #13 Granian RSGI | 120 | 39% | 22.3 | 926 |
| #1 FastAPI + uvicorn (droplet: 11k) | 130 | 41% | 7.3 | — |

**Bench limit: 130k users** (`just bench ladder 160000 --resolution 10000`; 140k passed once and
failed once). **CPU fails first**:

| Users | p99 | `core0_util` (softirq part) | Peak MiB | Reconnects in 3 min |
|---|---|---|---|---|
| 80k | 15–16 ms | 50% (14%) | 1,411–1,422 | 0.4–0.5M |
| 120k | 121 ms | 76% (23%) | 1,293 | 1.3M |
| 130k | 203 ms | 83% (26%) | 1,433 | 1.4M |
| 140k | 433–1,758 ms | 84–87% (28%) | 1,283–1,474 | 1.5–1.6M |
| 160k | 5,094 ms | 98% (32%) | 1,512 | 1.7M |

Past ~65k users the descriptor budget turns nearly every request into a new TCP connection, and
the kernel's share (softirq) grows to a third of the core. Memory is second (1.5 of 1.75 GiB at
160k). Descriptors and errors never failed (0 errors in every run).

- **At 80k and 140k** #84 stays at 51 µs/request (p99 17 ms at 140k, 61% CPU) and #71 at 54–69 µs
  (p99 407 ms at 140k); both pass 140k. README "Results" has the full table.
- **Droplet estimate: about 22k**, like #10, which costs the same per request.
- PR #84 claims 140–160k for itself and 40k for us, measured over loopback in a sandbox limited to
  20k descriptors. Its lead over us is real here too (~1.5× less CPU per request).

## Next: beat #84 without leaving FastAPI + Granian

1. **Optimise what fails first: CPU.** It fails first on the bench (above), and on a droplet's slower
   vCPU it fails sooner, at ~22k users, long before memory or descriptors. Target: µs per request,
   73–81 now, 50 for #84. Past 65k users, also the cost of each reconnect.
2. **Profile on Linux.** Add `--profile` to `just bench linux`: `py-spy record --native` inside the
   server container during the hold (`SYS_PTRACE` on the server in `main.tf`, behind a variable).
   Split the time into Python, Granian, SQLite and kernel before changing anything.
3. **Reconnects** dominate past 65k users (1.6M at 140k). Granian owns `accept`, so #84's tricks
   (`TCP_DEFER_ACCEPT`, `accept4`) aren't ours, but the profile will show their cost.
4. **Candidates, after asking the owner:** `--task-impl rust`; `asginl` (move lifespan work into
   `main.py`); Python 3.13/3.14 from deadsnakes in `install.sh` (Ubuntu ships 3.12; Granian has no
   PyPy support); Starlette routing cost; `MALLOC_ARENA_MAX` for Granian's ~12.5 KiB per connection.
5. Two real droplets would turn the estimate into a score (costs money; the owner's call).

## Environment (owner's Mac)

- `python3` is Apple's 3.9.6 (justfile Python must stay 3.9-compatible); Homebrew has 3.14.
- The shell is **nushell**; the justfile sets its own shell.
- macOS numbers mean nothing for capacity (listen backlog 128, 10 cores); use `just bench linux`.
- Two Colima VMs (server and generator apart) don't work: `vz` NAT drops connections past 5–10k.
- Colima profiles `bench-server` and `bench-gen` are stopped leftovers; delete if the owner agrees.
- `~/.docker/config.json` uses `credsStore: desktop`; `bench/linux.py` puts Docker.app's bin on `PATH`.
- With `locking_mode=EXCLUSIVE`, the `sqlite3` CLI can't open the DB while the server runs.
