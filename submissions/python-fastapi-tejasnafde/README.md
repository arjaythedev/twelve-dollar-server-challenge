# Python + FastAPI

| | |
|---|---|
| Language | Python 3.12.3 (Ubuntu 24.04's `python3` package) |
| Framework | FastAPI 0.142.2 (Starlette 1.7.0) |
| Server | uvicorn 0.54.0 with uvloop 0.23.0 (event loop) and httptools 0.8.0 (HTTP parser) |
| SQLite driver | the standard library `sqlite3` module, linked to Ubuntu's SQLite 3.45.1 |
| JSON | orjson 3.12.0 |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependency versions, transitive ones included, are pinned in `requirements.txt` (the same
pins as `python-fastapi-cknutson12`).

## Running it

```bash
sudo bash install.sh   # apt: python3, python3-venv
bash build.sh          # creates .venv and installs requirements.txt
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Starting point

This submission starts from [`python-fastapi-cknutson12`](../python-fastapi-cknutson12), which is
already well tuned: one uvicorn process with uvloop and httptools, no Nginx, orjson, no Pydantic
models, JWT checked by hand, WAL with mmap. The SQL, auth, pragmas, GC settings and uvicorn
settings are unchanged from it. The difference is how a request travels from uvicorn to a handler.

I profiled it with py-spy under load. About 40% of the Python CPU time was in FastAPI and Starlette
layers, not in the handlers. So I removed those layers one at a time and measured each step.

## What changed, and what each step bought

Each step adds one change to the step before it. "CPU per request" is the server's user + system
CPU time (all processes, from `/proc`) divided by the number of requests, at a fixed 10,000-user
k6 load for 2 minutes. Steps 0, 3 and 4 ran in one session. Steps 1 and 2 ran in an earlier
session and are shown as the change from step 0 in that session.

| Step | Change | CPU per request |
|---|---|---|
| 0 | cknutson12, unchanged | 0.447 ms |
| 1 | FastAPI telemetry off (`telemetry={"tracing": False, "metrics": False, "logs": False}`) | no measurable change |
| 2 | Router called directly; no `ServerErrorMiddleware` or `ExceptionMiddleware` | about -2% |
| 3 | Plain Starlette `Route`s on the FastAPI router: no FastAPI dependency solving | 0.330 ms (-26%) |
| 4 | Endpoints are raw ASGI apps: no `Request`, no `Response` class, no per-request exception wrapper | **0.287 ms (-36%)** |

Details of the final version (`app.py`):

- **No FastAPI dependency solving.** `@app.get` creates an `APIRoute`, which runs
  `solve_dependencies` and parameter validation for every request, even for a handler that takes
  one `str`. That was the single largest cost. The routes here are Starlette `Route`s added to the
  FastAPI app's router.
- **Endpoints are raw ASGI apps.** Starlette mounts an endpoint that is not a function as a raw ASGI
  app, so the small `Endpoint` class skips the `Request` object and the exception wrapper that
  Starlette builds for every request. Handlers read `scope["headers"]`, `scope["path_params"]` and
  the body from `receive` directly.
- **A two-call response.** `JSONResponse` sends `http.response.start` with two headers and then the
  body. Starlette's `Response` builds a header list through several methods for every response.
- **The middleware stack is the router plus one `try/except`.** It keeps every error in the API's
  JSON format: 404 for unknown paths (`router.default`), 405 for a wrong method, 500 for anything
  unexpected. `AsyncExitStackMiddleware` can go too, because only FastAPI `APIRoute`s need it.
- **The open-file limit is raised in the process.** Each keep-alive client is one socket, and
  Ubuntu's default soft limit is 1,024. `server.py` raises the soft limit to the hard limit.

## Results

Test bench: a GCE `e2-small` (2 vCPUs that share one core, 2 GB RAM, Ubuntu 24.04) as the server,
and a separate k6 VM in the same zone, using `bench/load.js` unchanged. An e2-small can burst above
its one core for a short time and is then throttled, which is much like a shared droplet CPU. Only
5-minute holds give a true limit on it; 2-minute runs look much better than they are.

5-minute holds, same session:

| Users | Version | p95 | p99 | Errors | Result |
|---|---|---|---|---|---|
| 9,000 | this submission | 1.6 ms | 66 ms | 0% | pass |
| 9,000 | cknutson12 | 826 ms | 954 ms | 0% | fail (p95) |
| 9,500 | this submission | 129 ms | 157 ms | 0% | pass |

In an earlier session, cknutson12 passed 8,000 users for 5 minutes and both versions failed 10,000.
Shared-core VMs are noisy, so treat the absolute numbers as rough. In every session, this version
used less CPU per request than cknutson12: 18% less in the 8,000-user hold, 36% less in the
2-minute runs.

## Tried, and not kept

- **Granian instead of uvicorn**, running the same app as ASGI: about 0.384 ms per request against
  0.311 ms for uvicorn at the same step. Granian is fast with its own RSGI interface, but its ASGI
  bridge costs more than uvicorn + httptools. Also note: Granian's default `backpressure` equals the
  backlog, so with more than 4,096 open keep-alive connections, requests wait until k6 times out.
- **Building the JSON in SQLite** (`json_object` + `group_concat(... ORDER BY ...)`): byte-identical
  output, but only about 2.6 µs faster per feed request, about 0.25% of the CPU per request.
- **A different feed query shape** (take the 20 newest posts first, then join): slower. The plan
  for the reference query is already an index scan, a primary-key join and a covering-index count.
  A query with no join and no count, which the spec does not allow, is only about 11 µs faster.

## Where the time goes now

py-spy at 9,000 users: about 35% SQLite (the feed query alone is 22%, because it is the most
frequent request and returns 20 rows), about 20% uvicorn's protocol code, about 18% the uvloop
event loop and socket work, and about 10% Starlette routing and the handlers. Further gains need a
different HTTP server, not FastAPI changes.

## License

MIT, under the repo's [license](../../LICENSE).
