# Node.js + uWebSockets.js

| | |
|---|---|
| Language / runtime | Node.js v24.21.0 LTS (official tarball, installed to `/opt/node-v24.21.0`) |
| HTTP server | [uWebSockets.js](https://github.com/uNetworking/uWebSockets.js) v20.71.0, **compiled from source** on the box: uWebSockets.js `127ac80`, uWebSockets `1d80f7a`, uSockets `86097c4` (exact commits) |
| SQLite driver | [better-sqlite3](https://github.com/WiseLibs/better-sqlite3) 13.0.3, **compiled from source** with its bundled SQLite 3.53.4 (the npm package's prebuilt addons are deleted first) |
| JSON | built by SQLite (`json_object`, `group_concat`) for reads |
| JWT | hand-rolled HS256 check with `node:crypto`, every request |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

One process, one thread. All versions are pinned: Node by version + SHA-256, the uWebSockets sources by
git commit, better-sqlite3 by `package-lock.json`.

## Running it

```bash
sudo bash install.sh   # build-essential, python3, git; Node.js 24.21.0 into /opt/node-v24.21.0
bash build.sh          # compiles uWebSockets.js (~1 min) and better-sqlite3 + SQLite (~2 min)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## How a request is served

- **Reads** (`/feed`, `/posts/:id`, `/health`) run synchronously inside the uWebSockets handler: one
  prepared statement returns the finished JSON body as a single string, which goes straight to
  `res.end()`. No row objects, no `JSON.stringify`, no `node:http` request/response objects or streams.
- **Writes** (`POST /posts`, likes) are **group-committed**. The handler verifies the JWT, validates the
  input and queues the write. At the end of the current event-loop turn (`setImmediate`) every queued
  write runs inside one `BEGIN … COMMIT`, and only after `COMMIT` returns are their responses sent. A 201
  always means the row is committed (rule 6). Because the transaction opens and commits inside a single
  synchronous function, no read ever runs while it is open, so reads never see uncommitted rows and are
  never delayed by writes.

## Optimizations, and why

Measured on an Apple Silicon dev box: the server in an Ubuntu 24.04 container pinned to **one core**
(2 GB), the load generator (wrk, or k6 with `bench/load.js`) in a second container on other cores,
both on the same loopback. Absolute numbers are much higher than the droplet will give. Only compare
numbers from the same table: each table's runs were taken back to back.

1. **uWebSockets.js instead of `node:http`.** On one vCPU the HTTP layer is most of the cost once SQLite
   is fast. uWebSockets parses HTTP in C++ and calls one JS function per request; there are no
   per-request JS objects, streams or events. With 20,000 k6 users connected, the whole process used
   about 112 MB RSS (see the comparison below).
2. **Built from source, with the keep-alive timeout fixed.** uWebSockets hard-codes a **10 s** HTTP
   timeout, applied both while a connection waits for its next request (`HTTP_IDLE_TIMEOUT_S`) and
   after each response (`HTTP_TIMEOUT_S`). A k6 user is idle for 8–23 s between requests, so with
   the stock build most users lost their connection every iteration and had to reconnect. Some
   requests also failed when the server closed a socket just as k6 reused it, and Go's HTTP client
   doesn't retry those POSTs. `build.sh` patches both constants to 75 s (SPEC asks for ≥ 65 s).
   k6, 20,000 users, same build otherwise:

   | | open connections | failed requests | p99 |
   |---|---:|---:|---:|
   | 10 s after each response (stock) | 12,000–17,000 (churning) | 307 (0.15%) | 3.67 s |
   | patched 75 s | 20,000 (stable) | 0 | 28 ms |

   Building from source also leaves out TLS and HTTP/3 (no BoringSSL / lsquic), and drops the
   `uWebSockets: 20` header from every response (`UWS_HTTPRESPONSE_NO_WRITEMARK`).
3. **SQLite builds the response JSON.** `json_object(...)` per post, `group_concat` for the feed, and
   the `{"posts":[…]}` / `{"post":…}` wrapper concatenated in SQL. `POST /posts` builds its response in
   the `INSERT … RETURNING` clause. One string crosses from SQLite into JS per request. Returning it as
   a BLOB (a Buffer for `res.end`) was measured too and lost: −12% on `GET /posts/:id`, since a V8
   Buffer costs more to create than an ASCII string.
4. **No `ORDER BY` inside the aggregate.** The feed's subquery scans the `(created_at DESC, id DESC)`
   index as a co-routine and hands rows to `group_concat` in order. Adding `ORDER BY` inside the
   aggregate as well makes SQLite sort the 20 rows again. Dropping it took a feed query from 27 to
   22 µs, and the mixed load from about 37,500 to 42,000 req/s. `test/test.sh` checks the order
   against the golden feed.
5. **`locking_mode = EXCLUSIVE`.** This process is the database's only user. Holding the lock means the
   WAL index lives in heap memory instead of the `-shm` mapping, and no transaction makes `fcntl` lock
   calls. +5% on the mixed load.
6. **Group commit** (above): +3% on the mixed load over one autocommit transaction per write. I also
   tried a 5–20 ms commit window to collect bigger batches. At k6 rates it saved no measurable CPU and
   only added latency, so a batch is simply whatever arrived in one event-loop turn.
7. **`wal_autocheckpoint = 4000`** instead of 1000. A checkpoint's two fsyncs block the one thread.
   Checkpointing every 16 MB of WAL means 4× fewer of them, and pages rewritten in between are copied
   to the database once. Process CPU under a saturating write-heavy load went from 87% busy (the rest
   waiting on fsync) to 95%, with about +8% throughput.
8. **JWT verification with a `KeyObject`.** `createHmac('sha256', buffer)` re-imports the raw key on every
   call. That showed up as ~4% of all CPU, in `getKeyObjectSlots`. A `KeyObject` created once
   (`createSecretKey`) removes it: about +5% on the mixed load. The signature is still verified on every
   request (rule 5); nothing about a token is remembered.
9. **`--max-semi-space-size=64`**: fewer young-generation GCs, +3%. Request handling allocates little
   (one body string per request), so this is a small effect.
10. **Connection budget.** uSockets doesn't back off when `accept()` fails with `EMFILE`: the listening
    socket stays readable, so the event loop would spin. The server reads its
    descriptor limit from `/proc/self/limits`, and above *limit − 1024* open connections it answers
    with `Connection: close`, so it stays under the limit instead of spinning.

### Tried and rejected

- **A checkpoint thread** (a `worker_threads` worker with its own connection running PASSIVE checkpoints,
  so fsyncs never block the event loop). It needs normal locking mode, which costs the 5% from (5).
  Under steady writes, PASSIVE checkpoints never catch up with the last frame, so the WAL never
  restarts: it reached 245 MB in 10 s. Forcing a RESTART fixed that, but then main-thread reads
  sometimes sat in SQLite's WAL read-retry loop. Under k6 that meant a 1 s event-loop stall and
  p99 617 ms, against 28 ms in exclusive mode.
- **Row arrays + JSON in JS** for the feed: 26 µs per query against 21.5 µs for SQLite-built JSON.
- **`json_quote` string concatenation or `format()`** instead of `json_object`: no faster.

## Measurements

**Read paths and mixed load** (wrk, 64 keep-alive connections, one pinned core, two runs; mixed =
`bench/load.js` proportions: 100 feed : 100 post : 15 like : 2 create, random ids and tokens):

| | req/s |
|---|---:|
| `GET /feed` | 41,800–51,200 |
| `GET /posts/:id` | 136,000–163,000 |
| mixed | 62,300–65,200 |

**Against the other Node.js submission** (#78: `node:net` with a hand-rolled HTTP parser, `node:sqlite`,
also SQLite-built JSON). k6 `bench/load.js` with 20,000 users, 30 s ramp and 60 s hold, both servers
started the same way on a fresh database. The server's CPU was capped at 20% of the core
(`cpu.max 2000 10000`) to bring both near their limit. Two runs each, in alternating order:

| | server CPU (same 207k requests) | RSS | p95 | p99 | errors |
|---|---:|---:|---:|---:|---:|
| this submission | 14% | 112 MB | 46–61 ms | 180–226 ms | 0 |
| `node:net` (#78) | 17% | 198 MB | 114–200 ms | 392–417 ms | 0 |

About 18% less CPU for the same requests, about 43% less memory with 20,000 open connections, and half
the tail latency.

## Rules

- **Rule 5:** every request reads SQLite while it is served. There are no response, row or JWT caches;
  only prepared statements, SQLite's page cache and `mmap` are reused.
- **Rule 6:** WAL with `synchronous = NORMAL`. A write's response is sent only after the `COMMIT` that
  contains it returns. If a statement or the commit fails, the affected requests get a 500 and nothing
  is reported as created.
- **Rule 11:** nothing outside the process is tuned. In-process settings: SQLite pragmas, the V8
  young-generation size, and uWebSockets' compile-time timeout constants.

## License

MIT, under the repo's [license](../../LICENSE). uWebSockets.js / uWebSockets / uSockets are Apache-2.0,
better-sqlite3 is MIT, SQLite is public domain; their sources are downloaded at build time and not
included here.
