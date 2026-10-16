# Python/Cython direct HTTP server

One native reactor owns the sockets, request storage, SQLite statements, and output
buffers. Python handles startup and configuration. Requests follow this path:

```text
epoll → recv → llhttp → ready ring → handler → SQLite → JSON chunks → sendmsg
```

The server speaks HTTP/1.1 directly, without Nginx, ASGI, or a Python web framework.
The target is Ubuntu 24.04 x86_64 with one vCPU, 2 GB RAM, and no swap.

## Build and run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/fresh/feed.db JWT_SECRET=twelve-dollar-challenge \
  HOST=0.0.0.0 PORT=80 bash start.sh
```

The service must supply `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE`. The process
runs as a normal user in the foreground. Use `HOST=127.0.0.1 PORT=3000` for local
checks. The four environment variables above configure the running server.

Build dependencies are pinned to Cython 3.1.6, setuptools 80.9.0, llhttp 9.3.0, and
yyjson 0.12.0. Parser source archives are SHA256-verified. Python, SQLite, and
OpenSSL use the Ubuntu 24.04 packages. The extension is compiled on the target.

## Where request overhead is removed

1. **Native execution and direct dispatch.** The reactor releases the GIL once
   and calls native Cython functions declared `noexcept nogil`. There are no ASGI
   scopes, middleware calls, dependency injection, Python request/response objects,
   or coroutine handoffs. Python still has a GIL; the request loop runs outside it.
   Routing uses direct method and path comparisons.

2. **HTTP parsing without application header dictionaries.** llhttp handles HTTP
   framing. Callbacks retain the method, URL, authorization value, and body in
   bounded storage. Other header values are not materialized into application
   objects. Protocol parsing and validation still happen.

3. **Bounded, reusable storage.** One 128 MiB arena supplies application request
   and output allocations through a buddy allocator. Requests use a 4 KiB block
   and reset only 48 bytes of metadata. Authorization and body buffers are acquired
   only when needed; idle connections hold neither request nor application I/O
   buffers. Application allocations avoid per-request `malloc` and `mmap` calls.
   SQLite and OpenSSL retain their own allocation behavior.

4. **Direct serialization and retained buffer ownership.** SQLite columns become
   JSON in linked 4 KiB chunks, without intermediate row dictionaries or
   `json.dumps`. Headers occupy reserved space in the first chunk. `sendmsg`
   gathers up to 32 chunks without flattening the response into another buffer.
   Partial sends retain ownership, and pipelined input resumes at its existing
   offset. Input accumulation and ordinary kernel TCP sends still involve copies;
   this is not end-to-end zero-copy.

5. **Bounded scheduling and fewer interest changes.** One thread collects up to
   128 epoll events and dispatches up to 64 ready requests per turn. The ready ring
   stays within that thread. Ordinary replies avoid unnecessary epoll interest
   changes; backpressure enables write notifications when needed. A timer wheel
   handles idle expiry without repeatedly scanning every connection. Connections
   remain alive for approximately 75 seconds, and TCP_NODELAY is enabled.

6. **Reuse setup while preserving fresh work.** SQLite opens once with `NOMUTEX`,
   and statements are prepared once. Every request still executes SQL. WAL with
   `synchronous=NORMAL` is used, and writes finish before success is exposed.
   Every authenticated request verifies its signature and claims. OpenSSL reuses
   the keyed HMAC context; there is no presigning ring or verification cache.
   Batching applies to event and dispatch work, not grouped write transactions.

These changes remove work from the request path. Their individual latency
contributions have not been isolated by the capacity benchmark.

## Memory locality

Connection records are 128 bytes, aligned to 64 bytes, with hot I/O fields in the
first cache line. HTTP parsers occupy a separate 96-byte-per-slot slab. Request
records use deterministic cache-line offsets within their arena blocks rather
than always starting at the same page offset. One owner avoids application worker
threads sharing mutable connection state.


## Measured performance

The frozen pre-cleanup build passed the unchanged challenge workload at **65,500
concurrent browsing users** on GCE `custom-1-2048`: one non-shared guest vCPU,
2 GiB allocated RAM, Ubuntu 24.04 x86_64, no swap. Both descriptor limits were
65,535. A separate machine generated load.

| Measurement | Result |
|---|---:|
| p95 request duration | 82.14 ms |
| p99 request duration | 242.84 ms |
| HTTP errors | 0% |
| Average HTTP requests/sec, including ramps | 6,090.93 |
| Nominal plateau process CPU | 38.93% |
| Sampled process high-water memory | 114.38 MiB |
| Peak open descriptors | 65,508 |

Each confirmation used a fresh process/database, a 1,000-user two-minute warm-up,
a 60-second ramp, a five-minute hold, and a 30-second down-ramp with 10-second
grace. VUs represent browsing users with think times, not requests per second.
Reported percentiles include the ramps.

k6 request duration measures sending, waiting, and receiving; it is not handler
CPU time and excludes initial DNS/connection setup. See the
[k6 metric definition](https://grafana.com/docs/k6/latest/using-k6/metrics/reference/#http-specific-built-in-metrics).
Synchronous SQLite calls, storage stalls, reactor queueing, scheduling, and
generator behavior can still contribute to the tail; these results do not isolate
their contributions.

This establishes a passing load near the descriptor ceiling, not the processing
maximum or an official DigitalOcean score. The refactored source has passed
functional and allocation checks; full capacity has not been rerun after the
refactor. [BENCHMARK.md](BENCHMARK.md) contains the detailed measurement record.

## Verification

The x86-64 release and ARM64 ASan/UBSan extension each passed 42 official checks
and 44 additional checks. Native ownership tests cover exhaustion, nonoverlap,
coalescing, serialization, partial sends, and cancellation. Negative controls
verify detection of overrun, use-after-free, and double-free. Pressure checks held
15,000 idle connections without drops and verified recovery after arena pressure
from 8,000 incomplete bodies, with no OOM event.

With the server running on a disposable seed copy:

```bash
bash ../../test/test.sh http://127.0.0.1:3000
python3 tools/check_edges.py http://127.0.0.1:3000 --db /path/to/disposable/feed.db
python3 tools/check_native.py
```
