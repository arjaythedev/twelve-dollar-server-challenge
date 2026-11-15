# Python + FastAPI

| | |
|---|---|
| Language | Python 3.12 (Ubuntu 24.04's `python3` package) |
| Framework | FastAPI 0.162.7 (Starlette 1.10.0) |
| Server | uvicorn 0.54.0 with uvloop (event loop) and httptools (HTTP parser) |
| SQLite driver | the standard library `sqlite3` module |
| **Nginx or direct** | **Behind Nginx**: listens on `127.0.0.1:3000` |

All dependency versions are pinned in `requirements.txt`.

## Running it

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Optimizations, and why

- **One process, one persistent SQLite connection, called inline on the event loop.** The box has
  one vCPU, so a connection per request (or a pool/threadpool) only adds setup and context-switch
  cost for no concurrency benefit. All handlers are `async def` and call `sqlite3` directly; since
  nothing is awaited during a query, no other request can interleave mid-query, so there's no need
  for locking around the shared connection.
- **Pragmas set once at startup**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 256 MiB
  `mmap_size`, 64 MiB page cache, `temp_store=MEMORY`.
- **uvloop + httptools**: the fastest event loop and HTTP parser uvicorn supports.
  `timeout-keep-alive 75` keeps idle connections open the 65 s Nginx needs (rule 9).
- **SQL**: the reference queries from SPEC.md. PyJWT verifies HS256 signatures with `exp` required,
  no caching of verifications (rule 5). Writes `commit()` before the response is sent (rule 6).

## License

MIT, under the repo's [license](../../LICENSE).
