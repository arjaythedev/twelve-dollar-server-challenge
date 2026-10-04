# C++ + uWebSockets + SQLite

Experimental native submission with 384 application lines, including authentication and Unicode
helpers. Serves directly with `HOST=0.0.0.0 PORT=80`. This starts from the
[C submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/5) and changes the
HTTP layer while retaining the SQL, database settings, JSON serialization, and HMAC implementation.

Four pinned libraries are bundled from source:

| Component | Version |
|---|---|
| uWebSockets: HTTP and routing | `e2ec2d7af011203420e782fb649d27934fc2bdea` |
| uSockets: native epoll/kqueue networking | `86097c490263ab662d62e8e7b541390bdec7d149` |
| fio-stl: buffers, escaping, base64 and HMAC; HTTP disabled | `24a57015d0989c64d5cc08ea9d24bd6cf97848bb` |
| SQLite: database and strict JSON parsing | 3.53.4 |

`build.sh` SHA256-verifies all downloads. TLS, compression and uWebSockets branding are disabled;
there is no OpenSSL, zlib or libuv dependency. C++ code uses Clang because fio-stl's C99 initializer
extensions are not supported by GCC's C++ frontend. SQLite and uSockets use the C compiler.
The executable links the platform C/C++ runtime libraries.

```bash
sudo bash install.sh                 # Ubuntu 24.04 toolchain, once
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Use a fresh copy of the seed database. From the repository root:

```bash
bash test/run.sh submissions/cpp-uwebsockets-tbsvttr
```

Additional tests against an already-running server use Python's standard library:

```bash
JWT_SECRET=secret python3 submissions/cpp-uwebsockets-tbsvttr/check.py http://127.0.0.1:3000
JWT_SECRET=secret python3 submissions/cpp-uwebsockets-tbsvttr/transport_check.py 3000
```

The transport checks use localhost, write posts and likes, and take at least 66 seconds to verify
reuse of the same idle connection. Run tests separately from benchmarks.

## Design

- One event loop owns one SQLite connection, its prepared statements, and a reusable JSON buffer.
  Every read queries current SQLite data. Every authenticated request verifies its JWT.
- WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 64 MiB page-cache limit, and a
  256 MiB mmap limit. Writes use autocommit and complete before the HTTP response.
- Native row serialization retains the C implementation's field order and escaping. uWebSockets
  copies any response bytes that need buffering, allowing the shared output buffer to be reused.
- POST callbacks own authentication headers and body storage while awaiting fragmented uploads.
  Aborted requests release that storage; bodies over 16 KiB receive 413 and connection closure.
- The pinned uWebSockets version hardcodes two HTTP timeouts to 10 seconds. The build applies a
  checked, idempotent patch setting both to 75 seconds to satisfy the challenge. No other library
  source is patched. Socket read limits and backpressure otherwise use uWebSockets defaults.
- The process attempts to raise its descriptor soft limit to the existing hard limit. There is no
  thread pool, query/result cache, JWT verification cache, or write batching.

## Validation

All 42 official checks passed on macOS ARM64. The additional checker passed 62 HTTP checks plus
16 simultaneous identical likes, covering strict JSON, Unicode boundaries and whitespace,
authentication, expiration, current reads, and concurrent post ordering. Acknowledged writes
survived `SIGKILL` and restart; this tests process recovery, not power-loss durability.

Transport checks passed fragmented and chunked uploads interleaved with other requests,
100 aborted uploads, 1,800 pipelined responses under backpressure, and same-socket reuse after
66 idle seconds. Application, uWebSockets and fio helper code also passed integration and
transport checks with AddressSanitizer and UndefinedBehaviorSanitizer. The sanitizer build used
the system allocator for fio and release objects for SQLite and uSockets.

SQLite uses the first duplicate JSON object key; the spec does not define duplicate-key behavior.
Decoded lone surrogates are rejected. The official Ubuntu build and droplet score remain untested.

## Local comparison, 2026-10-04

Compared against the C submission on the same macOS ARM64 host with both servers
compiled at `-O3`, byte-identical SQLite object files, and identical SQL, JSON escaping, authentication,
and database settings. This targets the HTTP layer but also changes the application compiler
frontend from C to C++.

Each trial starts with a fresh copy of the seed, with one wrk thread, 64 keep-alive connections,
a two-second warm-up and an eight-second measurement. Three trials per workload, alternating
server order. The mixed workload uses 100 feed reads, 100 post reads, 15 likes and two creates
per 217 requests on average, without think time or independent state per virtual user.

Median requests/second across the three trials:

| Workload | C + fio | C++ + uWebSockets | Difference |
|---|---:|---:|---:|
| Feed reads | 34,483 | 36,087 | +4.7% |
| Single-post reads | 118,826 | 131,647 | +10.8% |
| Mixed reads and writes | 43,736 | 45,394 | +3.8% |

No HTTP or socket errors were reported. Single-post C++ runs ranged from 124,419 to 137,966 req/s,
so the small number of local trials should not be treated as a precise forecast for the droplet.

| Median trial p99 | C + fio | C++ + uWebSockets |
|---|---:|---:|
| Feed reads | 2.14 ms | 3.44 ms |
| Single-post reads | 0.747 ms | 0.763 ms |
| Mixed reads and writes | 8.19 ms | 7.92 ms |

| Size / dependencies | C + fio | C++ + uWebSockets |
|---|---:|---:|
| Application source including helpers | 351 lines | 384 lines |
| Bundled third-party libraries | 2 | 4 |
| macOS ARM64 executable | 2.12 MiB | 1.37 MiB |
| Process RSS after mixed load (`ps`, median) | 52.1 MiB | 50.8 MiB |

This version improves local throughput modestly and produces a smaller executable, but adds code
and dependencies and has worse read-tail latency. RSS excludes kernel socket memory and does not
represent total system memory cost. Neither implementation wins every measure.

To repeat the mixed test against an already-running server, from the repository root:

```bash
wrk -t1 -c64 -d2s -s submissions/cpp-uwebsockets-tbsvttr/bench.lua http://127.0.0.1:3000
wrk -t1 -c64 -d8s --latency -s submissions/cpp-uwebsockets-tbsvttr/bench.lua http://127.0.0.1:3000
```

For read-only workloads, omit `-s` and use `/feed` or `/posts/500000`. The load generator shares
the server machine; neither the droplet's CPU limit nor its idle-connection population is reproduced.
These throughput measurements are not challenge scores. Python and wrk are optional validation
tools, not application dependencies.

## License

Application code is MIT under the repository's [license](../../LICENSE). uWebSockets and uSockets
are Apache-2.0, fio-stl is MIT, and SQLite is public domain; their source distributions retain their licenses.
