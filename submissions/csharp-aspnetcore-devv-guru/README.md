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
| SQLite driver | Microsoft.Data.Sqlite 10.0.13 |
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
  authentication, body parsing and response writes run outside it. Readers and
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
  with a two-second admission deadline. Queued
  requests await a permit without holding a database session or worker thread.
  Queue rejection and other unexpected errors use the spec's generic JSON 500.
- **SQLite-generated JSON.** Feed and post queries use the existing indexes and
  SQLite's JSON functions to produce the response in UTF-8. Casting the result
  to a BLOB lets the driver return bytes directly, avoiding per-post .NET
  objects, UTF-16 strings and a second JSON encoding pass. Every request executes
  its query against current data; no result bytes are retained between requests.
  Likes use `ON CONFLICT DO NOTHING` for idempotency.
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
the additional **49/49** exclusive-access checks on Linux. They cover JSON
escaping and ordering, Unicode, concurrent writes, fresh API reads, exclusive
ownership, rollback, commit visibility after an immediate process kill, restart
and offline backup/restore. The publish completed with
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
| Pooled readers, initial PR #195 | 40,000 | 3,523 | 225.95 / 455.24 ms | 0 | Pass |
| Same preserved build | 45,000 | 3,926 | 464.10 / 1,257.75 ms | 0 | Fail |
| One connection, exclusive ownership | 45,000 | 3,986 | 357.68 / 700.80 ms | 0 | Pass |

**The single-connection build passed 45,000 users locally.** The matched 45k
baseline failed the p99 threshold. This is one full-duration pair: it supports
retaining the change for its observed tail latency, without establishing an
exact capacity ceiling or a large throughput improvement.

Central-hold server CPU averaged 82.9% for the baseline and 81.5% for the candidate.
Observed process RSS peaks were 723 and 728 MiB; cgroup peaks, including charged
page cache, were 1,429 and 1,440 MiB. Both had zero HTTP errors, swap and OOM kills.
Both databases passed post-load consistency/foreign-key checks and acknowledged
create reconciliation. The candidate contained all 15,315 acknowledged warm-up
and measured creates on top of the 500,000 seed posts.

These are same-host WSL diagnostics with k6 outside the server cgroup. The OS is
outside that memory limit, and CPU quotas/loopback traffic differ from the
target VM. Official scoring remains pending. The
[verification guide](checks/README.md#load-testing) includes the client resource
use, inconclusive short probes and earlier comparisons.

## License

MIT, under the repository's [license](../../LICENSE).
