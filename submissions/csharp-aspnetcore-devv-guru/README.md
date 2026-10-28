# C# / ASP.NET Core — devv-guru

A .NET 10 Minimal API implementing the [challenge API](../../SPEC.md) with
SQLite. Kestrel serves requests directly, without Nginx.

The goal is to push throughput using optimizations I would keep in a production
codebase. Each change should earn its place through measurement while keeping
the code readable, writes durable, authentication correct and failures observable.

## Stack

| Component | Version |
|---|---|
| C# / SDK | C# 14 / .NET SDK 10.0.401 |
| Runtime | .NET / ASP.NET Core 10.0.13, self-contained Linux x64 |
| SQLite driver | Microsoft.Data.Sqlite 10.0.13 |
| Native SQLite | SQLitePCLRaw.bundle_e_sqlite3 2.1.14; SQLite 3.54.0 in the tested build |
| JWT | Microsoft.IdentityModel.JsonWebTokens 8.23.0 |
| HTTP / JSON | Kestrel HTTP/1.1 / System.Text.Json |

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

Windows build commands and local benchmark setup are in the
[verification guide](checks/README.md).

## Implementation

- **Prepared SQLite sessions.** Read-only sessions and their commands are leased
  from the framework's `ObjectPool`. It retains two to eight idle sessions,
  depending on CPU count; excess sessions close when returned. A separate writer
  is used under one exclusive permit. Provider connection pooling is disabled
  because these sessions already own their connections. Readers close before a
  session returns to the pool, and failed sessions are discarded.
- **Short, durable writes.** Every write commits its own transaction with WAL,
  `synchronous=FULL` and foreign keys enabled before returning success. The write
  limiter queues up to 256 requests with a two-second admission deadline. Queued
  requests await a permit without holding a database session or worker thread.
  Queue rejection and other unexpected errors use the spec's generic JSON 500.
- **Indexed queries.** Feed and post reads use the existing schema and indexes.
  Likes use `ON CONFLICT DO NOTHING` for idempotency. Each request reads current
  SQLite data; prepared commands cache query plans, not results.
- **Framework request handling.** Endpoint modules group the health, feed and
  post routes. Typed responses use generated JSON metadata with the required
  field order. Body size is limited to 16 KiB; the 500-character limit counts
  Unicode code points. Keep-alive is 130 seconds.
- **Startup.** ReadyToRun publishing and generated request delegates reduce
  first-request work. Before listening, startup validates SQLite settings and
  engine compatibility, prepares commands and constructs JWT validation settings.
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
Challenge tokens have no issuer or audience. A real deployment needs its own
signing keys, issuer/audience policy, key rotation and HTTPS configuration.

## Verification

On 2026-10-14, a source-only Linux build passed the official **42/42** checks and
the additional **41/41** checks. The additional checks also passed on Windows.
They cover concurrent writes, external updates, rollback, bounded contention,
commit visibility, process-kill recovery and backup/restore. NuGet's configured
advisory source reported no known vulnerable packages, including transitive ones.

Run the additional checks after building, from the repository root:

```bash
python3 submissions/csharp-aspnetcore-devv-guru/checks/integration.py \
  --seed seed/feed.db \
  --server submissions/csharp-aspnetcore-devv-guru/bin/publish/FeedApi
```

### Local load results

Fresh servers/databases, a 1,000-user/two-minute warm-up, a 60-second ramp and a
five-minute hold. The server had a verified one-core CPU quota, 2 GiB memory
limit and no swap. Rates and latency include ramp/drain.

| Virtual users | Requests/sec | HTTP p95 / p99 | HTTP errors |
|---:|---:|---:|---:|
| 20,000 | 1,866 | 9.61 / 100.10 ms | 0 |
| 25,000 | 2,333 | 13.11 / 92.74 ms | 0 |
| 30,000 | 2,797 | 28.66 / 170.66 ms | 7,278 (0.650%) |

**25,000 was the highest load tested with zero errors.** At 30,000, the aggregate
challenge thresholds passed, but 8.28% of likes and 8.47% of creates failed;
write p99 reached about two seconds. All acknowledged creates were present in
the saved databases, which passed SQLite consistency and foreign-key checks.

These are same-host WSL diagnostics with k6 outside the server cgroup. The OS is
outside that memory limit, and CPU quotas/loopback traffic differ from the
target droplet. They establish neither an official score nor sustained
production capacity. [Benchmark details and reproduction](checks/README.md#load-testing)
include per-endpoint results and the load generator's resource use.

## License

MIT, under the repository's [license](../../LICENSE).
