# python-litestar-przwr

- Language: Python 3.12+ (tested on 3.12)
- Framework: Litestar 2.24.0 on uvicorn 0.54.0 (uvloop, h11)
- SQLite driver: stdlib `sqlite3`, one connection per worker process (thread-local), autocommit
- Mode: **direct** on 0.0.0.0:80 (no Nginx)

## Implementation notes

- All handlers are async; SQLite calls run inline on the event loop (queries are index-backed
  and sub-millisecond, so threadpool hops cost more than they save on 1 vCPU).
- JWT HS256 verification is done manually with `hmac`/`hashlib` on every request (rule 5), no
  caching of anything request-derived.
- WAL + `synchronous=NORMAL`, autocommit mode: an INSERT's commit completes before the 201 is
  written (rule 6). `mmap_size=256MB`, `busy_timeout=5s`, 8MB page cache per connection.
- Responses are msgspec Structs serialized by Litestar's default msgspec encoder: compact JSON,
  exact key order from struct field order.
- Keep-alive 75 s (works behind Nginx too, rule 9).
- `WORKERS` env (default 1). On the droplet's single vCPU, 1 uvicorn worker measured best.

## Local measurements (before submission, desktop core pinned with taskset, k6 on other cores)

- 42/42 correctness checks
- 5-minute hold, 27,000 VUs: p95 16 ms, p99 209 ms, 0.00% failed, ~2,520 rps, 327 MB RSS
- Local ceiling was k6-side ephemeral port exhaustion on loopback (errno 99), not the server
