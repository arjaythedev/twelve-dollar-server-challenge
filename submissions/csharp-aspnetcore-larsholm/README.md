# C# + ASP.NET Core (Minimal APIs)

| | |
|---|---|
| Language | C# 14 on .NET 10.0 (SDK 10.0.401, Microsoft's official tarball, pinned in `global.json`) |
| Framework | ASP.NET Core 10 Minimal APIs on Kestrel |
| SQLite driver | Microsoft.Data.Sqlite 10.0.12 (bundled SQLite via SQLitePCLRaw) with Dapper 2.1.89 |
| JSON | System.Text.Json (the framework default) |
| **Nginx or direct** | **Direct**: Kestrel serves `0.0.0.0:80` itself |

NuGet versions, transitive ones included, are pinned in `packages.lock.json`, and `build.sh` restores in locked mode.

## Running it

```bash
sudo bash install.sh   # apt: curl, libicu74; .NET SDK tarball into /usr/local/dotnet
bash build.sh          # dotnet publish -c Release -o bin/publish
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Approach

This is deliberately **ordinary C#**: it's written the way I'd write a small production API, not tuned for
the benchmark. It's the `dotnet new web` template with the endpoints added:

- Minimal API endpoints with `async` handlers, records for the response shapes, and the framework's JSON
  serializer with a `snake_case` naming policy. The `appsettings.json` logging levels come straight from the template.
- A new pooled `SqliteConnection` per request and Dapper for queries. The SQL is the spec's reference
  queries. There are no extra pragmas: the seed database is already in WAL mode, and SQLite's default in WAL is `synchronous=FULL`.
- Default runtime settings: Server GC (the template default), the default thread pool, and Kestrel's
  default limits. Kestrel has no connection limit by default and its keep-alive timeout is 130 s.
- **Direct**: Kestrel is built to face the internet, and on one vCPU Nginx's CPU comes straight out of the
  app's budget. The measurements below explain why I went direct.

The one non-default piece is JWT verification. `Microsoft.IdentityModel` rejects HMAC keys shorter than
256 bits, and the challenge secret is 184 bits, so `JwtValidator.cs` checks HS256 directly with
`HMACSHA256` and a constant-time compare, then checks `exp`, `sub` and `username`. Every request
verifies its token. Nothing is cached.

## My measurements

I ran `bench/load.js` (k6 v2.3.0) unchanged against a DigitalOcean Basic `s-1vcpu-2gb` droplet (Ubuntu 24.04, fra1).
k6 ran on two separate 4 vCPU / 8 GB droplets in the same region, over the private network. My account can't
create a bigger single load generator, and k6 needs about 250 KB per VU. Each load generator ran half the VUs
with its own slice of `tokens.json`. A run counted as passing only if both halves passed the thresholds, which is
slightly stricter than one combined run. Every 5-minute hold started on a fresh copy of the seed database.

**Score: 15,250 users** (5-minute hold, direct)

| Users | Hold | p95 | p99 | Errors | Result |
|---:|---|---:|---:|---:|---|
| 13,750 | 5 min | 36 ms | 303 ms | 0% | pass |
| 14,500 | 5 min | 29 ms | 313 ms | 0% | pass |
| 15,000 | 5 min | 112 ms | 456 ms | 0% | pass |
| **15,250** | **5 min** | **90 ms** | **574 ms** | **0%** | **pass** |
| 15,500 | 5 min | 153 ms | 1,130 ms | 0% | fail (p99) |
| 17,500 | 5 min | 391 ms | 1,439 ms | 0% | fail (p99) |
| 20,000 | 5 min | 251 ms | 1,886 ms | 0% | fail (p99) |

What I found:

- **Short runs overstate capacity.** In 2-minute search runs, 20,000 users passed (p99 330 ms) and 22,500 passed
  (p99 626 ms). Every 5-minute hold from 15,500 users up failed. Hold for the full 5 minutes before you trust a number.
- **The tail fails first, not the median.** Failing runs had medians of a few ms and 0% errors, but p99 went over
  1 s and the slowest requests took 3–30 s. Meanwhile the CPU kept about 10% idle. Requests waiting while the CPU
  idles suggests queueing, not a lack of CPU. My guess is thread pool starvation: Microsoft.Data.Sqlite's
  `*Async` methods run synchronously, so a write waiting on SQLite's lock blocks a pool thread. On one vCPU,
  .NET starts with a single thread pool thread and adds more slowly. I haven't verified this.
  `ThreadPool.SetMinThreads` would be the first thing to try.
- **The kernel takes a big share of the CPU.** With 15,000–20,000 open keep-alive connections, about 30–35% of
  CPU time was system time and about 50% was user time.
- **CPU steal on the shared vCPU** was usually under 2%, but it spiked to 20–40% for a few seconds in some runs.
  Close to the limit, one spike is enough to fail a run.
- **Memory is not the limit.** The .NET process used about 120 MB idle and 300–490 MB at 15,000–30,000 users.

**Direct instead of Nginx**: I benchmarked behind `bench/nginx.conf` first. In 2-minute search runs it passed
13,750 users and failed 14,250 (p99 13 s), and there was no 5-minute hold. Near saturation the 64 pooled
upstream connections run out. Nginx then opens new ones, which queue in Kestrel's accept backlog and time
out after the 5 s `proxy_connect_timeout`, and that extra work feeds the overload. The Nginx error log showed
upstream connect timeouts and `worker_connections are not enough`. Serving directly, each client keeps its own
connection to Kestrel, and Nginx's CPU share goes back to the app.

For comparison: the original video's normal-code C# + **Postgres** version scored 4,400. The same plain code
style on in-process SQLite scores about 3.5× that. The gap to the hand-optimized C# submissions is what
allocation-free code buys.

## License

MIT, under the repo's [license](../../LICENSE).
