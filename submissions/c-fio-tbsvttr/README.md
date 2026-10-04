# C + fio-stl + SQLite

Experimental native submission: 351 lines of application C, including authentication and Unicode
helpers, with two bundled libraries. Serves directly with `HOST=0.0.0.0 PORT=80`.

| Component | Pinned version |
|---|---|
| HTTP, buffers, JSON escaping, base64, HMAC | [fio-stl](https://github.com/facil-io/cstl/tree/24a57015d0989c64d5cc08ea9d24bd6cf97848bb), commit `24a5701` |
| Database and strict JSON parsing | SQLite 3.53.4 |

`build.sh` downloads SHA256-verified sources and links them into one executable. No OpenSSL,
package-manager libraries, JavaScript runtime, or dynamically linked SQLite is required at runtime.
The local macOS ARM64 executable is 2.1 MiB and links only the system library. The fio-stl pin is
from its developing 0.8 branch; this is an experiment, not a claim of production maturity.

```bash
sudo bash install.sh                 # Ubuntu 24.04 toolchain, once
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Use a fresh copy of the seed database. From the repository root:

```bash
bash test/run.sh submissions/c-fio-tbsvttr
```

Additional tests against a seeded, already-running test server use Python's standard library:

```bash
JWT_SECRET=secret python3 submissions/c-fio-tbsvttr/check.py http://127.0.0.1:3000
```

## Design

- One reactor thread owns one SQLite connection and its prepared statements. No result cache,
  token-verification cache, thread pool, or write batching.
- WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 64 MiB page-cache limit, and a
  256 MiB mmap limit. Every successful write commits before its HTTP response.
- Rows are serialized straight into a reusable native buffer; fio copies the response before
  the application reuses that buffer. All reads query current SQLite data.
- Every write request verifies HS256, expiration, optional `nbf`, and required claims. Signature
  comparison is constant-time. SQLite parses JSON because fio's parser accepts nonstandard syntax
  and its Unicode escape handling is incomplete at the pinned revision.
- Small Unicode helpers enforce scalar UTF-8, JavaScript whitespace trimming, and code-point
  length limits. Raw NUL in JSON input is rejected before SQLite validation.
- 75-second keep-alive. fio raises the process descriptor soft limit to its existing hard limit.
  The default connection read buffer is 8 KiB; large idle-connection populations still use memory.

The reference SQL in `SPEC.md`, the single-connection design in
[PR #3](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/3), and our
[Bun submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/4) informed this experiment.

## Validation

All 42 official checks pass locally on macOS ARM64. Additional checks cover concurrent duplicate
likes, token expiration, invalid signed JWTs, Unicode boundaries, strict JSON, current reads, and
concurrent post ordering. An instrumented build passed the same checks with AddressSanitizer and
UndefinedBehaviorSanitizer enabled for application/fio code; SQLite used its release object.
Acknowledged writes also survived `SIGKILL` and restart. This checks process recovery, not power loss.

UTF-8 decoding was separately checked against every Unicode scalar and its truncated encodings.
SQLite returns the first duplicate JSON object key; Bun returns the last. The spec does not define
duplicate-key behavior. Decoded lone surrogates are rejected rather than emitted as invalid UTF-8.

Ubuntu execution and the official five-minute droplet benchmark have not been measured.

## Local comparison, 2026-10-04

Both compiled executables use a fresh copy of the seed
for each trial on the same macOS ARM64 host, with one wrk thread, 64 keep-alive connections, a
two-second warm-up, and an eight-second measurement. Three trials per workload; server order
alternates. The mixed workload uses 100 feed reads, 100 post reads, 15 likes, and two creates per
217 requests on average, without think time or independent state per virtual user.

Median requests/second across the three trials:

| Workload | Minimal Bun | C | Difference |
|---|---:|---:|---:|
| Feed reads | 29,064 | 33,983 | +16.9% |
| Single-post reads | 83,141 | 118,744 | +42.8% |
| Mixed reads and writes | 33,351 | 43,534 | +30.5% |

No HTTP or socket errors were reported. Median mixed-load p99 was 8.28 ms for C versus 10.29 ms
for Bun. Median process RSS after mixed load, as reported by macOS `ps`, was 51.7 MiB versus
86.8 MiB; this is not a measurement of total memory including kernel socket buffers.

| Size / dependencies | Minimal Bun | C |
|---|---:|---:|
| Application source, including helpers | 105 lines | 351 lines |
| Compiled executable on macOS ARM64 | 59.3 MiB | 2.1 MiB |
| Bundled dependencies | Bun, no npm packages | fio-stl and SQLite |

C wins these local performance and executable-size comparisons, but takes over three times as
much application code. It does not beat Bun on every simplicity measure.

Individual endpoints can be measured with `wrk -t1 -c64 -d8s --latency http://127.0.0.1:3000/feed`
or `/posts/500000`, after a two-second warm-up. The included `bench.lua` is identical to
[the Bun submission's benchmark script](https://github.com/tbsvttr/twelve-dollar-server-challenge/blob/5244d8fe567b45bbb1894da99192a96d29b3f921/submissions/typescript-bun-tbsvttr/bench.lua).
To repeat the mixed workload against an already-running test server, from the repository root:

```bash
wrk -t1 -c64 -d2s -s submissions/c-fio-tbsvttr/bench.lua http://127.0.0.1:3000
wrk -t1 -c64 -d8s --latency -s submissions/c-fio-tbsvttr/bench.lua http://127.0.0.1:3000
```

`wrk` and Python are optional validation tools, not application dependencies.

These are local throughput measurements, not challenge scores. The load generator shares the
server machine, and neither the droplet's CPU limit nor its idle-connection population is reproduced.
C uses bundled SQLite 3.53.4; the local Bun 1.4.2 executable uses macOS SQLite 3.51.0, so this measures
the complete implementations rather than isolating language or HTTP-library overhead.

## License

MIT, under the repository's [license](../../LICENSE). fio-stl is MIT licensed; SQLite is public domain.
