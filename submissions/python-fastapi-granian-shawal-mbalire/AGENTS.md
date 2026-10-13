# AGENTS.md: python-fastapi-granian-shawal-mbalire

Handoff notes for the next agent. Read this file in full before you touch any code. The source is the source of truth;
this file holds the why and the rules.

## Goal

Build a submission for the $12 Server Challenge (repo root: `../../README.md`, `../../SPEC.md`).
The server runs on 1 vCPU and 2 GB of RAM, and the score is the most concurrent k6 users it holds for 5 minutes.
The entry must:

- **use FastAPI, served by Granian** (a Rust HTTP server). Keep FastAPI; the owner asked for it.
- **have SQLite build the response JSON.** Python never builds response JSON; it only joins byte strings.
- **borrow the best ideas from the other languages' entries** (listed below).
- **follow hexagonal architecture.** Use the owner's `hexagonal-architecture` skill.
- **include a justfile** that follows the `justfile` skill. **Developers must not need uv**: use `python3 -m venv` and pip.
- **be easy to profile.** One command should produce a flamegraph or a cProfile dump under load.

Git: branch `python-fastapi-granian-shawal-mbalire` in a local clone of
`arjaythedev/twelve-dollar-server-challenge`. Don't commit, push, fork or open a PR until the owner asks. To submit, the owner forks the repo and opens a PR that adds only this folder.

## Hard rules (from README.md and SPEC.md, summarised)

- SQLite opened in-process at `$SQLITE_PATH`. The schema, indexes and triggers can't change.
- **No caching across requests.** Every request reads SQLite, and every JWT is verified.
  Prepared statements, SQLite's page cache and mmap are allowed.
- **Writes must be durable before the response is sent.** That means WAL with `synchronous=NORMAL` or stronger.
  Group commit is allowed if each response waits for its commit.
- Must pass all 42 checks in `test/test.sh` byte for byte. JSON key order matters, and output must be compact.
- Required files: `install.sh` (root, run once), `build.sh` (normal user, pinned deps),
  `start.sh` (foreground; configured by the env vars `SQLITE_PATH`, `JWT_SECRET`, `HOST`, `PORT`),
  `README.md`, the source, and `.gitignore`. Healthy within 60 s of start.
- Serve either directly on `0.0.0.0:80` or behind the Nginx config on `127.0.0.1:3000`. Plan: **direct**.
  The README must say which.

## Decisions and why

Each optimisation, with its source PR, is listed in `README.md` under "Optimizations". The ones that
are easy to break:

- **Granian flags live in `infra/ops.py` (`GRANIAN_FLAGS`).** `--backpressure 65535` is critical:
  Granian's default (backlog ÷ workers = 4096) caps *open connections*, and in direct mode every user
  holds one, so with the default, 20k users got 22% timeouts on an idle CPU.
  `--http1-header-read-timeout` maxes out at 60000 ms.
  The interface is `asgi`, not `asginl`, because the lifespan starts and stops resources.
- **Routes are raw ASGI endpoints** (`app.middleware_stack = app.router`). Don't add FastAPI
  dependencies, `Request` objects or middleware; PR #10 measured −36% CPU from skipping them.
- **`telemetry={...: False}` on `FastAPI(...)`.** Since FastAPI 0.143, each request otherwise does three
  `os.environ` lookups through opentelemetry.
- **SQLite renders every success body** (`json_object`, `text_factory = bytes`). Python only joins bytes.
- **Group commit** in `adapters/sqlite_posts.py`: writes await the batch's commit, and reads made while a
  batch is open await it too. Never send a write's response before its commit.
- **`adapters/fd_budget.py`**: above 40k open descriptors, responses carry `Connection: close` (PR #13).

## Layout (hexagonal)

```
main.py      composition root: settings → adapters → FeedApi → create_app → gc freeze
domain/      rules, ports, models, the five workflows; standard library only
adapters/    sqlite_posts, jwt_hs256, clock, asgi_http (driving), fd_budget, cprofile_profiler
infra/       config.py (Settings from env), ops.py (build/start; stdlib, Python 3.9-compatible)
tests/       stdlib unittest with fakes for every port
bench/       loadgen.py (load.js clone), linux.py (droplet-sized Docker bench), compare.py + stress.js
install.sh   one apt-get line (it must install python3, so it cannot shim into Python)
build.sh start.sh   one-line `exec python3 infra/ops.py <cmd>` shims
```

Request order (from SPEC): `POST /posts` checks auth → JSON decode → body rules; likes check
auth → id → store. A missing or malformed bearer header gives `MISSING_TOKEN`. A bad signature or
format, or a failed exp/nbf check, gives `INVALID_TOKEN`. A bad `sub` (including above 2^63−1) or
`username` gives `INVALID_TOKEN_PAYLOAD`.

## Scripts and justfile

- **The owner does not want shell scripts.** Put logic in Python (`infra/ops.py`, `bench/*.py`, or
  inline `#!/usr/bin/env python3` justfile recipes). The `.sh` files are required by the challenge and
  run with bash, so they stay one line each and must not grow.
- Justfile Python runs under Apple's 3.9 (`/usr/bin/env python3`), so keep it 3.9-compatible.
- No uv. `just setup` finds a Python ≥ 3.12 itself. Dev tools (py-spy) go in `.devtools/`, because
  `build.sh` runs `venv --clear` on `.venv`.
- `just --list` is the reference for recipes. It follows the `justfile` skill: `@` on every line,
  target dispatch (`just bench local|linux|ladder|compare`, `just vm start|stop|status`).

## Verification checklist

1. `just test`: unit tests pass.
2. `just check`: all 42 checks of `test/test.sh` pass. This is the CI gate.
3. `just vm start`, then `just bench linux <users>`: capacity under the droplet's limits.
4. `just profile cprofile` (and `just profile` with sudo for py-spy) to find CPU.
5. Ask the owner before these A/B experiments: SQL JSON vs orjson, group commit on vs off, `asgi` vs
   `asginl`, `--task-impl rust`.

## Environment gotchas (owner's Mac)

- `python3` is Apple's **3.9.6**; Homebrew has `/opt/homebrew/bin/python3.14`. The pins in
  `requirements.txt` also install on Ubuntu 24.04's Python 3.12.3 (verified in the Docker bench).
- The interactive shell is **nushell**. Bash-only syntax fails there, but the justfile sets its own shell.
- Seed: `../../seed/feed.db` and `tokens.json`. Rebuild with `bash ../../seed/make-seed.sh`.
- **Don't trust capacity numbers measured on the Mac.** macOS caps the listen backlog at 128
  (`kern.ipc.somaxconn`) and has 10 cores. Use `just bench linux`.
- `just vm start` switches the Docker context to `colima-bench`. Switch back with `docker context use colima`.
- With `locking_mode=EXCLUSIVE`, the `sqlite3` CLI can't open the DB while the server runs.

## Status (2026-10-13)

- `just test`: 28/28 pass. `just check`: 42/42 pass.
- Docker bench with droplet limits and 60 s holds (README "Results"): 20k, 40k and 80k users pass
  (80k: p99 99 ms, 931 MiB). 120k fails: the CPU saturates and memory reaches 1,745 of 1,792 MiB.
  Under overload, memory is the risk (OOM means losing).
- Next ideas: lower CPU per request (about 78 µs at load); bound memory under overload with a lower fd
  budget and a smaller `--http1-buffer-size`; run `just bench ladder` with 5-minute holds.
