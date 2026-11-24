# rust-epoll-fexaop

Front: **direct**. Rust 1.94, edition 2024, Linux epoll, and in-tree SQLite
3.53.4 through raw C bindings. No HTTP framework, async executor, ORM or
external crate beyond pinned `libc`, `httparse`, `memchr`, `itoa`, `base64`,
`sha2` and `hmac`.

The SQLite engine and HTTP framing started from the MIT-licensed
`rust-epoll-scooter1337` submission. This submission replaces its shared
`Mutex<App>` request path with an ordered request channel: network threads never
wait on a database lock.

## Request path

```
epoll reactor (N threads, SO_REUSEPORT)
  parse HTTP, verify JWT, validate body
        |
        | Job { fd, generation, close, op }   one FIFO mpsc channel
        v
single SQLite worker thread
  drain up to 128 jobs in arrival order
  BEGIN
  execute every job in that order
  COMMIT
  queue one Reply per job, in that order
        |
        | Reply { status, body }              per-reactor mpsc channel
        v
reactor receives through eventfd, frames the HTTP response, writes the socket
```

- **No request waits on a mutex.** `std::sync::mpsc` is the only hand-off. The
  SQLite connection is owned exclusively by the worker thread, so
  `SQLITE_THREADSAFE=0` is valid and there is no lock convoy, priority
  inversion, or reactor blocked inside SQLite.
- **Ordering is explicit.** Every parsed request — including auth errors,
  malformed JSON, 400s and `100 Continue` — becomes a `Job`. The worker consumes
  the channel in FIFO order, and each reactor receives replies in that same
  order. HTTP/1.1 pipeline responses therefore stay in request order per
  connection.
- **Async from the network side.** A reactor enqueues and continues reading,
  accepting or writing other connections. The worker wakes it with an eventfd;
  the reactor never polls a database condition or spins on a lock.
- **Group commit before any response.** A drained batch runs under one deferred
  SQLite transaction. The worker commits before queuing replies; if `COMMIT`
  fails, every non-raw response in that batch is replaced with a 500, except
  `/health`, which keeps the result of the `SELECT 1` it already ran. This keeps
  the rule that a 201 means committed. After notifying the reactors the worker
  yields, so on a one-core box the epoll loop gets to write the batch's replies
  before the next batch is drained.
- **Bounded queues and backpressure.** Each connection has at most 8 jobs in
  flight. At the cap its read interest is removed; completing replies re-enable
  it. Output above 1 MiB closes the client. The input cap is 1 MiB.
- **Reads and writes share the FIFO.** This is deliberate: a feed read queued
  after a create observes that create, and no connection can jump the queue.
  The trade-off is that reads do not run on parallel SQLite connections; the
  design targets the 1-vCPU scoring box, where one ordered worker avoids lock
  contention and repeated transaction setup.

`REACTORS` is optional (default 1; set it to 4 to run four epoll reactors).
All reactors feed the same ordered worker.

Other details retained from the base: WAL with `synchronous=NORMAL`, a 500-page
cache, 1 GiB mmap, `fdatasync`, a 4096-frame `RESTART` checkpoint after commit,
prepared statements, live feed range scans, JWT verification on every request,
and 120-second cleanup for idle/partial connections.

## Build and run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` reads only `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT`; `REACTORS`
is an optional in-process tuning knob.

## Verification

- Official `test/test.sh`: 42/42 on a fresh seed copy with 1 and 4 reactors.
- 24 pipelined `POST /posts` on one socket: responses returned in request order
  with sequential IDs and exactly 24 committed rows.
- Mixed pipeline (`200`, `200`, `400`, `404`, `200`, `Connection: close`) and
  `Expect: 100-continue` both preserved response order.
- 16 concurrent clients x 50 creates: 800 unique IDs, 800 committed rows.
- `REACTORS=4` uses four epoll reactors and the same ordered SQLite worker.
