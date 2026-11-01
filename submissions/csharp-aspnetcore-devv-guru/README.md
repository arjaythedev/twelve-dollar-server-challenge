# C# / ASP.NET Core — devv-guru

A .NET 10 Minimal API implementing the [challenge API](../../SPEC.md) with
SQLite. Kestrel serves requests directly, without Nginx.

The goal is the highest leaderboard score we can reach within the challenge
rules. [PR #95](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/95)
is the baseline; this follow-up prioritizes throughput, concurrency and tail
latency while keeping the required API behavior.

## Stack

| Component | Version |
|---|---|
| C# / SDK | C# 14 / .NET SDK 10.0.401 |
| Runtime | .NET / ASP.NET Core 10.0.13, Native AOT Linux x64 |
| SQLite driver | Microsoft.Data.Sqlite 10.0.13; SQLitePCLRaw 2.1.14 for feed/post reads |
| Native SQLite | SQLitePCLRaw.bundle_e_sqlite3 2.1.14; SQLite 3.54.0 in the tested build |
| JWT | Microsoft.IdentityModel.JsonWebTokens 8.23.0 |
| HTTP / JSON | Kestrel HTTP/1.1 / SQLite JSON for reads, System.Text.Json for writes |

The SDK is pinned in `global.json`; `install.sh` verifies its archive SHA-512.
NuGet dependencies are locked in `packages.lock.json`.

## Build and run

Run these commands from the repository root on Ubuntu 24.04 x86_64. The upstream
seed and test scripts also require Node.js 18+, sqlite3, jq, curl and openssl.

```bash
sudo bash submissions/csharp-aspnetcore-devv-guru/install.sh
bash seed/make-seed.sh
bash test/run.sh submissions/csharp-aspnetcore-devv-guru
```

`test/run.sh` builds the application and tests it against a fresh database copy.
To run the published server locally afterward:

```bash
run_dir=$(mktemp -d)
cp seed/feed.db "$run_dir/feed.db"
SQLITE_PATH="$run_dir/feed.db" JWT_SECRET=twelve-dollar-challenge \
  HOST=127.0.0.1 PORT=3000 bash submissions/csharp-aspnetcore-devv-guru/start.sh
```

The application reads four environment variables:

| Variable | Requirement / default |
|---|---|
| `SQLITE_PATH` | Required; an existing SQLite database |
| `JWT_SECRET` | Required; at least 32 UTF-8 bytes, except for the fixed challenge secret |
| `HOST` | IP address; defaults to `127.0.0.1` |
| `PORT` | Port number; defaults to `3000` |

The benchmark deployment uses **direct Kestrel on `0.0.0.0:80`**. Its runner
provides the low-port capability and `LimitNOFILE=65535`. `start.sh` runs in the
foreground, and SIGTERM uses ASP.NET Core's graceful shutdown.

Native build details and local benchmark setup are in the
[verification guide](checks/README.md).

## Implementation

- **One prepared SQLite connection.** The singleton repository owns one open
  connection and all prepared commands. A short lock serializes database access;
  authentication, body parsing and network flushing run outside it. Readers and
  transactions finish before releasing the lock. A failed session is discarded
  and recreated with the same settings on the next request.
- **Exclusive database ownership.** `locking_mode=EXCLUSIVE` is set before the
  first WAL access. SQLite retains its file locks instead of reacquiring them
  around each transaction. Other connections cannot access the database while
  the server owns it; stop the server for inspection, external edits or backups.
- **WAL with `synchronous=NORMAL`.** Every write commits its transaction before
  returning success. This removes the extra WAL flush on each commit, as allowed
  by rule 6. Acknowledged writes can be lost after a power failure or system
  crash. Foreign keys remain enabled. The write limiter queues up to 256 requests
  with a two-second admission deadline. An available permit is acquired without
  creating a linked cancellation source or timer. Queued requests await a permit
  without holding a database session or worker thread.
  Queue rejection and other unexpected errors use the spec's generic JSON 500.
- **SQLite-generated JSON.** Feed and post queries use the existing indexes and
  SQLite's JSON functions to produce the response in UTF-8. Casting the result
  to a BLOB lets a persistent SQLitePCLRaw statement copy UTF-8 into the response
  buffer, avoiding an intermediate response array, ADO.NET reader and per-post
  .NET objects. The statement finishes and resets before output is advanced;
  network flushing happens after releasing the database lock. Every request
  executes its query against current data; no result bytes are retained between requests.
  Likes use `ON CONFLICT DO NOTHING` for idempotency.
- **Stable query plans.** `SQLITE_DBCONFIG_ENABLE_QPSG` is enabled on every open
  connection. It avoids bound-value-driven replanning of the indexed ID lookups.
  This retains execution plans, not query results; every request reads SQLite.
- **Framework request handling.** Endpoint modules group the health, feed and
  post routes. Writes, health and errors use explicit generated JSON type
  metadata; read JSON uses the same field order in SQL. Body size is limited to
  16 KiB; the 500-character limit counts Unicode code points. Keep-alive is 130
  seconds.
- **Socket scheduling.** Socket completions and request continuations can run
  inline on I/O threads. Connections allocate receive buffers immediately,
  trading idle-connection memory for fewer scheduling and receive operations.
  The minimum request-body data-rate timer is disabled for this benchmark profile.
- **Native AOT.** The publish compiles to native code with speed optimization
  and invariant globalization. Request delegates and remaining JSON serializers
  are generated at build time. Before listening, startup validates SQLite
  settings and engine compatibility, prepares commands and constructs JWT
  validation settings.
  The content root is the executable directory, so starting from `/` does not
  cause configuration watching to scan the filesystem root.
- **Diagnostics.** Logs retain errors without logging each successful request.
  The `DevvGuru.FeedApi` meter exposes query/commit timings, write admission and
  thread-pool gauges. Collection is optional; see the verification guide.

### Authentication

IdentityModel validates HS256 signatures and expiration on every authenticated
request, with zero clock skew. Claim types are checked against the original JWT
JSON to reject numeric `sub` values and non-string usernames.

The public `twelve-dollar-challenge` secret is shorter than IdentityModel's HS256
minimum. A per-key crypto-provider override accepts only that exact fixture
secret and HS256; other secrets use the standard provider and minimum size.
Challenge tokens have no issuer or audience.

## Verification

On 2026-10-27, the Native AOT build passed the official **42/42** checks and
the additional **52/52** exclusive-access checks on Linux. They cover JSON
escaping and ordering, Unicode, concurrent writes, fresh API reads, exclusive
ownership, rollback, commit visibility after an immediate process kill, restart
and offline backup/restore. A feed over 60 KB verifies complete escaped/Unicode
bodies, ordering and the exact UTF-8 `Content-Length`. The publish completed with
warnings treated as errors under a one-core / 2 GiB / no-swap build limit.
The same-day NuGet audit reported no known vulnerable direct or transitive
packages; whitespace verification passed.

Run the additional checks after building, from the repository root:

```bash
python3 submissions/csharp-aspnetcore-devv-guru/checks/integration.py \
  --seed seed/feed.db \
  --server submissions/csharp-aspnetcore-devv-guru/bin/publish/FeedApi \
  --database-access exclusive
```

### Original submission

The maintainer benchmarked PR #95 on the leaderboard VM at **28,000 users held
for five minutes**, 2,747 requests/sec, 216 ms p95, 788 ms p99 and 0.02% errors.
The application used 67% CPU and peaked at 510 MB RAM. All 190 HTTP 500s were on
writes. [Official benchmark comment](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/95#issuecomment-6445292399).

### Local comparison: 2026-10-27

Fresh servers/databases, a 1,000-user/two-minute warm-up, a 60-second ramp and a
five-minute hold. The server had a verified one-core CPU quota, 2 GiB memory
limit and no swap. Rates and latency include ramp/drain.

| Build | Virtual users | Requests/sec | HTTP p95 / p99 | HTTP errors | Thresholds |
|---|---:|---:|---:|---:|---|
| Earlier exclusive-connection build | 45,000 | 3,986 | 357.68 / 700.80 ms | 0 | Pass |
| Stable plans + direct UTF-8 reads | 45,000 | 3,957 | 43.96 / 106.93 ms | 0 | Pass |
| Same final build | 47,500 | 4,123 | 425.04 / 1,076.27 ms | 0 | Fail (p99) |
| Same final build | 50,000 | 4,351 | 334.16 / 1,042.58 ms | 0 | Fail (p99) |

**45,000 users passed the five-minute hold locally with the final build.**
The 47,500- and 50,000-user attempts completed without HTTP errors but missed
the p99 limit. The lower-load retry had worse p99, showing local run variability;
these attempts do not establish a capacity increase over the earlier build.
Central-hold server CPU averaged 63.9% of one core; observed process RSS
peaked at 699 MiB and cgroup memory at 1,031 MiB, including charged page
cache. The client peaked at 11,866 MiB RSS. No app/client swap or OOM kills
were observed. The stopped database passed consistency/foreign-key checks and
contained all acknowledged creates (515,691 posts including the seed).
The earlier 45k build averaged 81.5% CPU. This single pair shows lower observed
CPU and tail latency with similar request throughput.

Separate short probes found +31.6% mean throughput for stable plans plus the
write fast path, and +3.6% for direct reads against that version. The allocation
probe recorded approximately 3,636 versus 1,721 managed bytes/request for the
direct reader. These are small diagnostic comparisons, not capacity predictions.

These are same-host WSL results with k6 outside the server cgroup and a CPU quota,
which differ from the target VM and a remote client. They confirm this tested
load rather than an exact ceiling or an official score. The
[verification guide](checks/README.md#load-testing) includes the previous matched
45k comparison, profiling, rejected changes and resource limitations.

## License

MIT, under the repository's [license](../../LICENSE).
