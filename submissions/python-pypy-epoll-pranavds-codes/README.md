# Python on PyPy, raw epoll, no framework

| | |
|---|---|
| Language | Python (3.9 dialect) on **PyPy 7.3.15**, Ubuntu 24.04's `pypy3` package |
| Framework | none: the HTTP/1.1 server is a `select.epoll` loop in `server.py` (one file, ~600 lines with the API) |
| SQLite driver | PyPy's standard-library `sqlite3` (cffi), linked to Ubuntu's SQLite 3.45.1 |
| JSON | built by SQLite (`json_object`), returned as `BLOB` bytes; stdlib `json` only parses request bodies and JWT payloads |
| Third-party packages | **none** (PyPy's bundled cffi calls `accept4()` and OpenSSL's one-shot `HMAC()`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

```bash
sudo bash install.sh   # apt: pypy3=7.3.15+dfsg-1build3
bash build.sh          # byte-compiles server.py (nothing to download)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Results (local, same harness for every entry)

Measured on one 4-vCPU Ubuntu 24.04 VM (Xeon @ 2.1 GHz), **not the official droplet**. Each server was pinned to
one CPU on a fresh seed copy; load came from the other three CPUs over loopback. Every number in this section
comes from the same harness, run on every entry.
PR numbers refer to [the challenge repo](https://github.com/arjaythedev/twelve-dollar-server-challenge/pulls).

### Users (capacity search, all Python entries)

`bench/load.js`'s user loop (feed, 3–7 s, one post, 3–8 s, 15% like, 2% create, 5–15 s), replayed by PR #13's
C load generator. Steps of 10k, 20k, 40k, 80k, 120k, 160k users, then one bisection. Each step uses a fresh
database, a 20 s warm-up, a 15 s ramp and a 45 s hold. A step passes with p95 < 500 ms, p99 < 1 s and < 1% errors.

| Python entry | Max users | p99 at max | Server CPU at max | RSS at max |
|---|---:|---:|---:|---:|
| **this submission** | **140,000** (fails at 160k) | 694 ms | 85% | 263 MB |
| #13 Granian RSGI (tbsvttr) | 60,000 (fails at 80k) | 409 ms | 90% | 354 MB |
| #41 FastAPI + Granian (shawal-mbalire) | 40,000 (fails at 60k) | 359 ms | 62% | 371 MB |
| #10 FastAPI (tejasnafde) | 20,000 (fails at 30k) | 22 ms | 38% | 249 MB |
| #23 Litestar (przwr) | 20,000 (fails at 30k) | 573 ms | 55% | 246 MB |
| #1 FastAPI (cknutson12) | 20,000 (fails at 30k) | 30 ms | 59% | 309 MB |

That is **2.3× the users of the previous best Python entry** (#13). The last three fail on this sandbox's
20,000-descriptor limit rather than on CPU; on the box, with 65,535 descriptors, they would get further.

**Official profile** (`bench/load.js` loop, 60 s ramp, 5-minute hold, re-run after the final changes): **160,000 users pass** with p95 173 ms, p99 310 ms, 1 error in 5,059,796 requests, 84% server CPU and 323 MB peak RSS. At 170,000 users p99 was 1,075 ms, just over the limit. The machine ran somewhat faster in this later session, so the table above, measured for every entry in one session, is the like-for-like comparison.

Across languages, on the same harness, 140k users ties the best Rust entry (#19) and beats the best entry
of Fortran (100k), Kotlin (30k), Go, TypeScript/Bun, PHP/Swoole, Ruby, Dart, Erlang (20k each), C# (15k) and Java (< 10k).

### Throughput per CPU

wrk on 256 keep-alive connections, 10 s per run after a 20 s warm-up. Mixed = load.js proportions
(100 feed : 100 post : 15 like : 2 create). Two interleaved runs each (ours, #13, ours, #13); the table shows the mean.

| | #13 Granian | this submission | change |
|---|---:|---:|---:|
| Mixed req/s | 19,662 | **25,925** | **+32%** |
| CPU per mixed request, µs | 51.2 | **38.5** | **-25%** |
| Mixed p99, ms | 24.4 | **17.9** | **-27%** |
| `GET /feed` req/s | 14,920 | **17,468** | **+17%** |
| `GET /posts/:id` req/s | 42,448 | **66,194** | **+56%** |
| Mixed, new TCP connection per request (64 conns), req/s | 16,993 | **24,520** | **+44%** |
| Server memory per idle keep-alive connection (18,000 held), bytes | 10,340 | **262** | 39× less |
| RSS after the throughput runs, MB | 64 | 219 | +245% (PyPy JIT and GC nursery) |

## Where the time goes, and what each change bought

Profiles (`perf`, server CPU only) of the obvious "fast Python" design show the costs this server removes:

1. **No framework, no ASGI/RSGI.** One epoll loop over raw descriptors parses HTTP with `bytes.find`
   and slicing, routes with byte comparisons and writes each response with one `write()`. Pipelining,
   partial reads, partial writes (EPOLLOUT), chunked bodies, `Expect: 100-continue`, HTTP/1.0 and
   `Connection: close` are handled, a request with both `Content-Length` and `Transfer-Encoding` is refused,
   and a client that sends without ever reading is cut off at 1 MB + 16 KB of queued input (`bench/http_edge.py`, 25 checks).
2. **PyPy instead of CPython.** Under CPython this exact server only ties Granian (the interpreter is
   23% of CPU). Under PyPy the JIT shrinks Python to ~10% of CPU: +44% mixed throughput, +45% on
   single posts. What remains is SQLite (~43%) and the kernel's TCP stack (~38%).
3. **SQLite builds the JSON.** `json_object(...)` with `group_concat` and `CAST(... AS BLOB)` returns
   the finished body as bytes: no row tuples, no dicts, no serializer. The feed's 20 rows are taken
   by `LIMIT` in a subquery before the joins; that query is ~20% cheaper than PR #13's form.
4. **Connection recycling instead of a descriptor wall.** With one connection per k6 user, a direct
   server dies at `LimitNOFILE=65535`. Above a keep-alive budget of 25,000 open sockets, replies carry
   `Connection: close` and the socket is closed right after the write. The server closes first, so
   TIME_WAIT stays on the server and the client's port is free again. 25,000 also stays under Linux's
   default ephemeral port range (28,232) if the k6 machine dials from one address. The 65k cap
   becomes a CPU question, and a cheap reconnect makes that question cheap.
5. **Cheap reconnects.** `TCP_DEFER_ACCEPT` makes `accept()` return only once the request bytes have
   arrived. `accept4(SOCK_NONBLOCK)` (cffi) skips two `fcntl`s. The new socket is read and answered
   immediately, and when it is being retired it is closed without ever entering epoll: **4 syscalls**
   per request on a fresh connection (`accept4`, `read`, `write`, `close`), down from 8+.
6. **~260 bytes per connection.** No per-connection objects: a set entry, plus dict entries only
   while a request or reply is partial. Granian holds ~10.3 KB per keep-alive socket.
7. **HMAC via OpenSSL's one-shot `HMAC()`** through cffi into a reused buffer: 1.7 µs instead of
   PyPy's `hmac.digest` at 11.4 µs. PyPy's version also leaked ~2 KB per call until the next major GC
   (anonymous memory grew ~28 MB/s under a like-only load).
8. **`locking_mode=EXCLUSIVE`, `mmap_size=1 GiB`, `synchronous=NORMAL`, autocommit.** A single
   connection owns the database. Each write is its own committed transaction before its 201 (rule 6).
   `foreign_keys=ON` so a validly signed token for a missing user cannot create orphan rows.

Every request runs its own SQL, and every token's signature and `exp`/`nbf` are verified per
request (rule 5). A fast path for the standard `{"alg":"HS256","typ":"JWT"}` header skips
decoding that header (a pure function of its bytes), never the signature check.

### Tried, measured, rejected

| Idea | Result |
|---|---|
| SQLite 3.50.4 / 3.53.4 built with `-O3 -march=native`, `THREADSAFE=0`, `MEMSTATUS=0`, PGO | ~15-20% faster feed query in a microbenchmark, **no measurable end-to-end gain** under PyPy (feed medians 15.4k vs 15.3k vs 15.5k req/s). Not worth a download and compile on the box |
| Background checkpoint thread (`wal_autocheckpoint=0`, second connection) | Slower: −8% mixed throughput, ~4× syscall time per create. On one core the work does not disappear, and it costs exclusive locking |
| Bigger `wal_autocheckpoint` (4,000-16,000 pages) | +13-19% on create-only load, but p99 rose to 96 ms (create-only) and 200 ms (mixed): fewer, bigger stalls. Kept SQLite's 1,000 |
| Granian / uvloop / any ASGI | The framework layer is the cost being removed; see the table above |
| Group commit for likes/creates | Writes are ~8% of requests; savings estimated at under 1% of CPU. Not worth the latency coupling |
| Feed as range scans in SQL (newest 20 ids, then one `BETWEEN` scan of likes with `GROUP BY`) | Same output; 17% faster on the fresh seed (98 → 82 µs) but 4.7× slower (331 → 1,543 µs) once recent posts collect likes, from SQLite's temp B-tree for the `GROUP BY`. Kept the correlated count |

### Considered and deliberately not done

- **Coalescing concurrent identical reads** (one feed query answering every `/feed` request in the
  same event-loop batch) would cut SQLite's share several-fold. It shares one query result between
  requests, which reads as a query-result cache under rule 5, so it is left out. Ask the maintainer first.
- **A hand-written C extension** for the hot loop would be faster still, but then it is a C submission.

## Caveats

- Loopback benchmarking charges the client's TCP receive path to the server CPU (about 20% of the
  server's kernel time in the profiles) and hides NIC/interrupt costs. Absolute numbers on the droplet will
  differ; the ratios between servers are what these tables show.
- The sandbox's 20,000-descriptor limit forced more recycling than the droplet will (budget 19,744
  here, 25,000 there). It penalizes every entry; the FastAPI and Litestar entries fail on it outright.
- A JWT without `exp` is accepted, as by jsonwebtoken, PyJWT and golang-jwt. PR #13's stricter
  extra checks flag that one case; every other one of its 78 HTTP checks plus 16 concurrent likes passes.
- PyPy RSS starts at ~200 MB (JIT and GC nursery) and stayed flat over the 5-minute holds.

## Reproducing

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/python-pypy-epoll-pranavds-codes                 # 42/42
cd submissions/python-pypy-epoll-pranavds-codes
bench/throughput.sh . mix 256 10            # needs wrk; CLOSE=close for a connection per request
bench/capacity.sh . 140000 300 60           # user model; needs cc; shards of 16k users
python3 bench/http_edge.py 3100 "$TOKEN"    # against a running server on a fresh seed copy
```

## Credits

[PR #13](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/13) (tbsvttr) introduced
SQLite-built JSON bytes and descriptor-pressure retirement for Python, and wrote `bench/connection_load.c`
(copied unchanged, MIT). [PR #1](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/1)
(cknutson12) and [PR #10](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/10)
(tejasnafde) set the SQL and the "skip the framework" direction.

MIT, under the repo's [license](../../LICENSE).
