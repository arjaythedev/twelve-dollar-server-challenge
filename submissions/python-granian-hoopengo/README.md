# Python 3.15 + Granian + APSW (async)

| Component | Version |
|---|---|
| Python | **3.15.0 final** (JIT enabled) |
| ASGI server | Granian 2.7.8+ (Rust) |
| Event loop | uvloop (improved asyncio with io_uring in Python 3.15) |
| SQLite driver | APSW 3.54+ (**native async**) |
| JSON | Rendered inside SQLite (`json_object` / `group_concat`) |
| **Nginx or direct** | **Direct** (`0.0.0.0:80`) |

## Best practices carried over from PR #41 and #13

### From PR #41 (python-fastapi-granian-shawal-mbalire)
- **`--backpressure 65535`** — Granian's default (4096) caps open connections. In direct mode each user holds one connection, so with the default 20k users we saw 22% timeouts.
- **Group commit** — writes within a single event-loop pass share one `COMMIT`; each response waits for its commit. This reduces disk load.
- **`locking_mode=EXCLUSIVE`** — faster for a single process.
- **Descriptor guard** — above 40k open file descriptors responses carry `Connection: close`.

### From PR #13 (python-granian-tbsvttr)
- **SQLite renders JSON** — `json_object` / `group_concat` build the documents; Python only concatenates byte strings.
- **183 lines** — minimalism, no extra abstractions.
- **75-second keep-alive** — holds connections longer than the standard 60 seconds.
- **Descriptor guard at 40k** — sampling no more often than once per 100ms.

## Python 3.15 optimizations
- **JIT compiler** — 8–9% speedup on x86-64 Linux (enabled via `PYTHON_JIT=1`).
- **Improved asyncio** — new scheduler with `io_uring` batch submission.
- **`frozendict`** — for constants in `auth.py`.

## Why Python 3.15.0 final, not just "3.15"

`granian` 2.8.4 is a Rust extension built on PyO3 against the **final 3.15 ABI**: its binary links against `PyType_FromSlots`, a symbol added to CPython only after alpha (present in 3.15.0b3 and in the final release, absent in 3.15.0a5).

If you install Python with the loose version `3.15`, uv picks the first matching managed build — **3.15.0a5** — and importing granian fails before the server even starts:

```
ImportError: dlopen(.../_granian.cpython-315-darwin.so, 0x0002):
  symbol not found in flat namespace '_PyType_FromSlots'
```

Therefore:
- `install.sh` installs from deadsnakes **`python3.15` 3.15.0-1+noble1** (final);
- `build.sh` creates the venv via `uv venv --python 3.15.0 --python-preference system` — exact version plus preference for the system interpreter;
- `build.sh` ends by checking that `sys.version_info[:3] == (3, 15, 0)` and running `import granian` — so this ABI error fails at build time, not at runtime;
- `start.sh` launches `.venv/bin/granian` directly, not via `uv run`, which could re-resolve Python to the alpha managed build.

## Bugs fixed

1. **`ImportError: symbol not found in flat namespace '_PyType_FromSlots'`** — see above.
2. **`ImportError: cannot import name 'queue_write' from 'db'`** — `app.py` imported `queue_write`, which does not exist in `db.py`. The error was hidden by the first bug (the server crashed earlier) and surfaced immediately after it was fixed. The unused import was removed; group commit is already provided by the APSW connection, so no separate function is needed.
3. **Spaces in JSON literals** — `test.sh` compares responses byte-for-byte against `test/golden/*.json`, while the SQL queries built `{"posts": [` and `{"post": `. Spaces after `:` / `[` were removed; empty feed is now `{"posts":[]}`.

## Running

```bash
sudo bash install.sh          # installs python3.15 (final), venv, build-essential
bash build.sh                 # creates .venv, installs dependencies, checks ABI
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```
