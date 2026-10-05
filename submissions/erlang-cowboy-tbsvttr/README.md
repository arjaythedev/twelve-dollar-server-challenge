# Erlang + Cowboy + SQLite

Experimental direct HTTP submission: `HOST=0.0.0.0 PORT=80`, without Nginx.
261 application lines in three Erlang modules. Cowboy handles HTTP connections; one Erlang
`gen_server` owns the SQLite connection and prepared statements. JWT verification and strict
JSON decoding use OTP's built-in `crypto` and `json` modules.

## Build and run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` builds the pinned Erlang/OTP 29.1.1 runtime from verified source under
`/opt/erlang/29.1.1` on Ubuntu 24.04. This is a one-time toolchain installation. Its build uses
one job by default; `BUILD_JOBS` can override that. On macOS, use an existing OTP installation
(OTP 27 or newer), the C compiler, curl and unzip. Tests here used OTP 29.1.1.

Five external components are downloaded as source with pinned commits and SHA256 checks:

| Component | Version / commit |
|---|---|
| Cowboy | 2.19.0 / `79e3fb02b31d47af6e69e8f3ba18fba291a3072a` |
| Cowlib | 2.20.0 / `69a047e5fff0232b27c347a64a644f7185638488` |
| Ranch | 1.8.1 / `616ce1566986ee704b9be36445a144f6c1c9c40c` |
| esqlite | 0.8.8 / `58454af87559981aed6fb1235fc3f63d618505db` |
| SQLite | 3.53.4 |

`build.sh` compiles the Erlang dependencies with `erlc` and builds the esqlite NIF against our
pinned SQLite source, replacing the driver's bundled SQLite version. No Rebar3 or build plugins
are needed. OTP is an additional runtime dependency and its crypto module uses OpenSSL.
This is not a standalone native executable.

## Design

- One normal BEAM scheduler, one dirty CPU scheduler, one dirty I/O scheduler, and one async
  worker. The esqlite driver runs SQLite steps on a dirty scheduler. Those are separate OS
  threads: choosing one normal scheduler does not itself impose a one-core CPU limit.
- The database owner serializes access to prepared statements. Its state contains no response
  or query-result cache. Every read queries SQLite; every authenticated request checks its
  token's signature and expiration again.
- WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 64 MiB page-cache limit and
  256 MiB mmap limit match the C++ submission. SQLite uses `THREADSAFE=1`, required by the driver,
  while the single-threaded C++ build uses `THREADSAFE=0`.
- SQLite builds each read response as one JSON column to avoid a separate Erlang/native
  scheduler transition for every feed row. The feed retains the indexed ordered `LIMIT 20`
  query and live like counts. JSON property order follows the specification.
- Inserts with `RETURNING` reach completion before acknowledging success. Likes also finish
  their autocommit transaction before responding. There is no write batching.
- HTTP idle/request timeouts are 75 seconds. The connection limit is 20,000; `start.sh` raises
  the descriptor soft limit to the current hard limit. There are no kernel settings or CPU
  affinity changes.
- POST bodies are limited to 16 KiB. Validation counts Unicode code points and uses the exact
  JavaScript trim characters. Invalid UTF-8 and unpaired surrogate escapes are rejected.

## Validation

From the repository root, with the seed available:

```bash
bash test/run.sh submissions/erlang-cowboy-tbsvttr
```

Optional checks against an already-running server use Python's standard library:

```bash
JWT_SECRET=secret python3 submissions/erlang-cowboy-tbsvttr/check.py http://127.0.0.1:3000
JWT_SECRET=secret python3 submissions/erlang-cowboy-tbsvttr/transport_check.py 3000
```

They write posts and likes. The transport check takes at least 66 seconds; run tests separately
from benchmarks. All 42 official checks passed on macOS ARM64 and native Linux ARM64 in the
official OTP 29.1.1 container. The Linux image uses Debian 13; the Ubuntu runtime installation
script and the official x86_64 droplet score have not been validated here.

On both platforms, 62 additional HTTP checks and 16 concurrent duplicate likes passed, covering strict
JSON, Unicode, authentication, expiration, concurrent creation and current reads. Acknowledged
writes survived `SIGKILL` and process restart; this verifies process recovery, not power-loss
durability. Direct database tests also covered empty feeds, failed-write recovery and query plans.

Transport checks passed fragmented/chunked uploads, 100 abandoned uploads, 1,800 pipelined
responses with a slow reader, and reuse of the original socket after 66 idle seconds. A separate
test held 15,000 keep-alive sockets while health checks succeeded. The Erlang process used
183.8 MiB RSS after that health-only traffic, excluding kernel socket memory and a populated
SQLite page cache. This is a connection-capacity check, not a concurrent-user challenge score.

## Local comparison

The baseline is the [C++ submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/6)
at `ba691aab9df3730cae0d37470ca1ea03ab83530e`. Both implementations use SQLite 3.53.4 and the same
database pragmas. This compares complete implementations: the SQL JSON assembly, driver,
threading, runtime and HTTP server all differ.

Measurements use native Linux ARM64 in Docker's Linux VM on the same Apple ARM64 machine.
The server container is restricted to one virtual CPU and 2 GiB RAM. A separate load-generator
container runs on other virtual CPUs and shares its loopback network namespace, avoiding Docker
port forwarding. This CPU placement belongs only to the comparison environment; the submission
scripts do not set CPU affinity. Both containers still share the host and Linux VM.

Each workload uses a fresh seed copy per run, one wrk thread, 64 keep-alive connections,
two seconds of warm-up and eight seconds of measurement. Three trials alternate server order.
The mixed script averages 100 feed reads, 100 post reads, 15 likes and two creates per 217 requests.
It uses live post IDs and the seed's 20,000 tokens, with no think time or per-user state.

Median requests/second across three trials:

| Workload | C++ + uWebSockets | Erlang + Cowboy | Erlang difference |
|---|---:|---:|---:|
| Feed reads | 35,124 | 11,576 | -67.0% |
| Single-post reads | 228,662 | 18,253 | -92.0% |
| Mixed reads and writes | 52,110 | 13,096 | -74.9% |

All 18 measured runs completed without reported HTTP or socket errors.

| Median trial p99 | C++ + uWebSockets | Erlang + Cowboy |
|---|---:|---:|
| Feed reads | 3.56 ms | 16.30 ms |
| Single-post reads | 0.534 ms | 15.69 ms |
| Mixed reads and writes | 13.93 ms | 17.51 ms |

| Code / dependencies / memory | C++ + uWebSockets | Erlang + Cowboy |
|---|---:|---:|
| Application source including helpers | 384 lines | 261 lines |
| Bundled third-party components | 4 | 5 |
| Process RSS after mixed load (median) | 53.8 MiB | 106.1 MiB |

The Erlang version has 32% fewer application lines but requires an additional component and the
OTP runtime. In this comparison the C++ implementation has about four times the mixed throughput
and uses about half the process memory. This result applies to these implementations and workload;
it is not a general ranking of the languages. RSS excludes kernel socket memory.

An additional paired run kept 15,000 idle HTTP connections open while 64 active connections
ran the mixed workload, again with a fresh database, two seconds of warm-up and eight seconds
of measurement. Every original idle socket successfully served another health request afterward.

| With 15,000 additional idle connections | C++ + uWebSockets | Erlang + Cowboy |
|---|---:|---:|
| Requests/second | 50,544 | 13,558 |
| p99 | 17.79 ms | 17.25 ms |
| Process RSS after load | 57.4 MiB | 270.2 MiB |

Both runs completed without HTTP or socket errors and took 8.04–8.05 seconds. This is one
exploratory pair, not a repeated median or a challenge score. A prior run with anomalous elapsed
time was discarded. The many-connection scenario did not reverse the throughput result.

To reproduce the mixed load against an already-running server, from the repository root:

```bash
wrk -t1 -c64 -d2s -s submissions/erlang-cowboy-tbsvttr/bench.lua http://127.0.0.1:3000
wrk -t1 -c64 -d8s --latency -s submissions/erlang-cowboy-tbsvttr/bench.lua http://127.0.0.1:3000
```

For read-only workloads, omit `-s` and use `/feed` or `/posts/500000`. Python and wrk are optional
validation tools, not server dependencies. These tests are not the official k6 challenge score.

## License

Application code is MIT under the repository's [license](../../LICENSE). Cowboy, Cowlib and Ranch
use ISC; esqlite and Erlang/OTP use Apache-2.0; SQLite is public domain. Downloaded distributions
retain their license files.
