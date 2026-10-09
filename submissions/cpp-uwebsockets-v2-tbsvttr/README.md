# C++ + uWebSockets: group commit

Direct HTTP submission: `HOST=0.0.0.0 PORT=80`, without Nginx. This builds on
[C++ submission #6](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/6)
at `ba691aab9df3730cae0d37470ca1ea03ab83530e`.

## Build and run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=secret HOST=0.0.0.0 PORT=80 bash start.sh
```

Ubuntu 24.04 uses Clang and LLD. The build uses link-time optimization across the
application, SQLite and uSockets. No architecture-specific CPU flags or runtime CPU
pinning are used. Start with a fresh seed database. The server is single-threaded.

Four external libraries are downloaded as source with pinned revisions and verified SHA256 hashes:

| Library | Version / revision |
|---|---|
| uWebSockets | `e2ec2d7af011203420e782fb649d27934fc2bdea` |
| uSockets, through uWebSockets | `86097c490263ab662d62e8e7b541390bdec7d149` |
| fio-stl, buffers / JSON escaping / base64 / HMAC | `24a57015d0989c64d5cc08ea9d24bd6cf97848bb` |
| SQLite | 3.53.4 |

There are three direct dependencies and four resolved libraries, using the same
package boundary as #6. The platform C/C++ runtimes and build tools are excluded.
TLS and compression libraries are disabled. Application code consists of
`server.cpp`, `auth.h`, and `unicode.h`: **432 physical lines**, including comments
and blank lines, versus **384** in #6. The baseline inventory counted **1,170 physical lines** including its build/start
scripts and validation/benchmark code. Documentation, recorded
benchmark data and downloaded libraries are excluded from that source total.

## Changes and transaction behavior

- Requests arriving before the next event-loop deferred callback share one SQLite
  transaction. There is no intentional timer delay and no response or query-result cache.
  Every authenticated request verifies its JWT; every read queries SQLite.
- Responses produced while the transaction is open own their serialized bytes and wait
  for `COMMIT`. This includes reads that observed the transaction's writes. A successful
  write response is sent only after the commit has completed. Failed commits roll back
  and turn queued responses into HTTP 500 errors.
- Disconnected clients invalidate their pending response pointers. Their already executed
  writes may still commit, as can happen with any request whose response is lost.
  Constraint failures are returned as errors without undoing other successful statements
  in the same transaction. No database schema, indexes, or triggers are changed.
- WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, and the 256 MiB mmap limit
  are retained. The SQLite page-cache target is reduced from 64 MiB to 8 MiB; mmap and
  other allocations are additional to that target.
- SQLite's unused memory-status tracking, progress callbacks, shared-cache support,
  declared-column-type API, deprecated APIs, and double-quoted string literals are
  disabled. These build options follow the applicable
  [SQLite compile-option guidance](https://www.sqlite.org/compile.html).
- JSON serialization, Unicode validation, JWT validation, and the indexed read queries
  retain #6's implementation. SQL-generated response JSON was measured during development
  and was slower for the mixed workload, so it is not used here.

HTTP idle/request timeouts remain 75 seconds. The process raises its descriptor soft
limit to the current hard limit. Kernel settings and CPU affinity are unchanged.
Sequential HTTP/1.1 keep-alive is supported. uWebSockets rejects another pipelined request
on a connection while an asynchronous response is pending; clients should await each
response before issuing another request on that connection. Read-only synchronous
pipelining was also checked, including slow readers.

## Connection pressure — 2026-10-09

At ordinary connection counts, keep-alive retains the 75-second timeout. At
60,000 open connections, completed responses advertise `Connection: close`
and the transport drains the response before closing its socket. Clients reconnect
for their next request. Uploads finish and successful writes commit before this
response-based retirement takes place. The threshold is a connection/memory budget,
not a limit on the number of logical users. Counters store transport metadata.

The HTTP connection filter counts opens and closes. The keep-alive budget leaves 128 descriptors free when the inherited file limit is smaller.

The source accounting and performance tables recorded before this change describe
the pinned baseline. New diagnostic evidence is in [capacity-results.json](capacity-results.json).
The selected local ARM64 diagnostic passed at **70,000 logical users**
for a **1-minute hold**, with zero errors and a worst-shard p99 of
**16.6 ms**. Peak process RSS was **52.7 MiB**;
sampled peak container memory was **575.9 MiB**. This is
one diagnostic trial, not a maximum-capacity search or an official droplet score.

From the repository root, against a running server on a fresh database copy:

```bash
python3 submissions/cpp-uwebsockets-v2-tbsvttr/tests/connection_check.py 3000 --users 512 --seconds 20 --ramp 2 --timeout 5
```

For descriptor pressure, run the server with hard/soft `nofile=256` and give the
client a higher limit. The diagnostic uses the feed/post/like/create loop and think
times from `bench/load.js`, measures the hold, and reports request latency plus
latency including reconnects. It uses Python, separate loopback source addresses,
and no warm-up or ramp-down; it is not an official k6 score. It writes posts and likes.

For sustained runs on Linux, compile the socket generator. It retains only each
user's selected post ID, uses deferred ephemeral-port allocation on several
loopback addresses, and lets the server's FIN retire completed connections. The
body text and random-number generator differ from k6; the request mix, user loop
and think-time ranges match. It reports generator CPU, memory and loop delay:

```bash
cc -O3 -Wall -Wextra submissions/cpp-uwebsockets-v2-tbsvttr/tests/connection_load.c -lm -o submissions/cpp-uwebsockets-v2-tbsvttr/bin/connection-load
submissions/cpp-uwebsockets-v2-tbsvttr/bin/connection-load 3000 --users 70000 --seconds 300 --ramp 60 --tokens seed/tokens.json
```

With the same low server limit, this targeted regression keeps an unfinished upload
open during connection retirement, verifies committed writes, and checks that
keep-alive resumes after the other connections close:

```bash
python3 submissions/cpp-uwebsockets-v2-tbsvttr/tests/pressure_regression.py 3000
```

## Validation

From the repository root, after generating the seed:

```bash
bash test/run.sh submissions/cpp-uwebsockets-v2-tbsvttr
python3 submissions/cpp-uwebsockets-v2-tbsvttr/verify.py
bash submissions/cpp-uwebsockets-v2-tbsvttr/tests/commit_check.sh
```

`verify.py` starts its own server and fresh database copy. It runs all 42 official
checks, 62 additional API checks, 16 concurrent duplicate likes, fragmented/chunked
uploads, 100 abandoned partial uploads, 100 reset connections after complete write uploads,
1,800 pipelined responses with a slow reader,
and reuse of the original socket after 66 idle seconds. A further 192 acknowledged
creates and their likes survived `SIGKILL` and restart, interleaved with 32 failed
foreign-key writes. This verifies process recovery, not survival of a power failure.

`tests/commit_check.sh` links a separate test executable with a fault-injection wrapper.
It verifies rollback on a failed commit, withholding success while a commit is blocked,
and successful writing after rollback. The wrapper is not part of the production build.

These checks passed on native Linux ARM64 / Ubuntu 24.04 in Docker Desktop. The official
Ubuntu x86_64 droplet and its five-minute k6 user-capacity score have not been measured.

## Measured comparison and weighted score — 2026-10-06

Both implementations were measured in alternating order on the same native Linux ARM64
setup: Ubuntu 24.04 in Docker Desktop, one server CPU and 2 GiB RAM, a separate load
container on other CPUs, shared loopback, and a fresh seed per run. Mixed-load figures
are medians of **five trials**, each with two seconds of warmup and **15 seconds** of
measurement. Every candidate mixed trial was faster and used less post-load RSS than
its paired baseline trial. All ten runs completed without reported HTTP/socket errors.

| Metric | Unmodified C++ #6 | This submission | Change |
|---|---:|---:|---:|
| Mixed requests/s | 59,471.80 | 71,375.95 | +20.0% |
| Mixed post-load RSS, MiB | 60.14 | 42.79 | -28.8% |
| Median trial p99, ms | 13.03 | 12.69 | -2.6% |
| Application LOC, including helpers | 384 | 424 | +40 lines |
| Direct / resolved library dependencies | 3 / 4 | 3 / 4 | unchanged |
| Recalculated weighted score / 100 | **85.8** | **94.0** | **+8.2 points** |

The read paths were also compared separately: three alternating trials per endpoint,
two seconds of warmup and eight seconds of measurement. All twelve runs completed
without reported HTTP/socket errors. The changes improve the mixed workload and feed
path, while single-post throughput is lower in this sample:

| Read-only workload, median requests/s | C++ #6 | This submission | Change |
|---|---:|---:|---:|
| Feed | 42,792.91 | 45,789.89 | +7.0% |
| Single post | 230,189.53 | 216,126.57 | -6.1% |

Raw outputs, executable hashes, resource samples, and source hashes are retained in
[`benchmark-results.json`](benchmark-results.json). They include every final measured
run; development probes are labeled separately and do not enter the final score.

A separate exploratory pair held **15,000 idle keep-alive connections** while 64 active
connections ran the mixed workload for eight seconds after two seconds of warmup:

| With 15,000 additional idle sockets | C++ #6 | This submission |
|---|---:|---:|
| Requests/s | 61,134.06 | 73,369.45 |
| Post-load RSS, MiB | 59.03 | 44.04 |

Every original idle socket successfully served another health request afterward. Both
runs completed without reported HTTP/socket errors. This is one capacity-check pair,
not a repeated median, and does not enter the weighted score.

The score keeps the [issue comment's](https://github.com/arjaythedev/twelve-dollar-server-challenge/issues/7#issuecomment-5995788219)
**40% performance, 20% application LOC, 20% resolved dependencies, and 20% RAM** weights.
For scoring, #6's throughput and RSS are replaced by the fresh paired measurements,
this candidate is added, and observed minima/maxima are recomputed across those 14 rows.
The other 12 historical rows retain their recorded inputs. Each component is linearly
normalized to 0–100; higher throughput and lower values for the other three metrics are
preferred. Missing RSS still produces no total score.

| Normalization input | Minimum | Maximum |
|---|---:|---:|
| Mixed requests/s | 3,550.52 | 71,375.95 |
| Application LOC | 105 | 1,354 |
| Resolved dependencies | 0 | 89 |
| Post-load RSS, MiB | 42.79296875 | 235.2734375 |

The exact inputs, bounds, component scores, and all 14 calculated rows are in
[`scoring.json`](scoring.json). This remains an illustrative preference index because
historical rows span different measurement environments. The previous **91.1** for #6
used earlier measurements and different bounds; compare **85.8 versus 94.0** under the
updated common calculation, rather than comparing 91.1 directly with 94.0. This is not
the official five-minute concurrent-user challenge score.

## Reproducing the comparison

`compare.py` runs each server on a fresh copy of the seed, warms it for two seconds,
and records throughput, latency output, process RSS/high-water RSS, CPU time, and the
executable SHA256. It preserves failed runs and stops on reported HTTP/socket errors.
The benchmark uses two wrk threads, 64 keep-alive connections, and the same `bench.lua`
for both servers. The mixed script averages 100 feed reads, 100 post reads, 15 likes,
and two creates per 217 requests, with live post IDs and the seed tokens. There is no
think time or independent virtual-user state.

Prepare two Ubuntu 24.04 containers with this repository mounted at `/work`. The server
container needs the build/test tools from `install.sh` plus Python 3, curl, jq, and OpenSSL;
the load container needs wrk. Build the candidate and unmodified #6 in the server container.
Limit the server container to one CPU and 2 GiB with no container swap. Assign different
CPUs to the load container and use `--network container:<server-container>` so requests
use shared loopback without Docker port forwarding. These CPU assignments belong to the
benchmark setup, not the submission scripts.

```bash
python3 submissions/cpp-uwebsockets-v2-tbsvttr/compare.py run \
  --server-container twelve-score-v2-server --load-container twelve-score-v2-loadgen \
  --baseline /baseline/submissions/cpp-uwebsockets-tbsvttr/bin/server \
  --workloads mixed --trials 5 --seconds 15 --output mixed-results.json
```

The baseline path is inside the server container. Choose a new output filename for each
run; existing evidence is not overwritten. `--workloads feed,post` measures the read paths.
The load generator and server share the host and Docker VM, so host activity can influence
results. Resource usage is process RSS sampled after load, excluding kernel socket memory;
it includes resident database mappings and is not total container RAM.

## License

Application code is MIT under the repository license. uWebSockets and uSockets use
Apache-2.0, fio-stl uses MIT, and SQLite is public domain. Downloaded sources retain
license files.
