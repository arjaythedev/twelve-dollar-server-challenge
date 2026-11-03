# C# / ASP.NET Core feed API

Implements `SPEC.md` using .NET 10 minimal APIs and Kestrel, with
Microsoft.Data.Sqlite **10.0.12**. The SDK is pinned to **10.0.401**;
`packages.lock.json` pins the transitive dependencies and builds use locked restore.

**Deployment: direct Kestrel, without Nginx or another reverse proxy.**
On the benchmark box, use `HOST=0.0.0.0 PORT=80`. Kestrel serves HTTP/1.1
on `HOST:PORT` and keeps idle connections alive for 130 seconds. No application
connection-count limit is configured, allowing the benchmark's approximately
15,000 keep-alive connections within the available resources. The challenge
provides `LimitNOFILE=65535` and `CAP_NET_BIND_SERVICE` for direct hosting.

## Build and run

On a clean Ubuntu 24.04 machine, run `sudo bash install.sh` once. With the
pinned SDK already installed, skip installation. From this folder:

```bash
bash build.sh
SQLITE_PATH=/absolute/path/to/fresh-feed.db \
JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`SQLITE_PATH` and `JWT_SECRET` are required. `HOST` defaults to `127.0.0.1`
and `PORT` defaults to `3000`. The database must already exist with the
repository's schema. The app never creates or alters tables or indexes.
`start.sh` runs the published app in the foreground.
For local development, use `HOST=127.0.0.1 PORT=3000` to avoid needing permission
to bind port 80. The official test runner sets these local values automatically.

From the repository root, generate the seed and run the official suite:

```bash
bash seed/make-seed.sh
bash test/run.sh submissions/csharp-aspnetcore-andrexx
```

Additional integration checks use only Python's standard library and a temporary
database. After building, run `python3 verify.py` from this folder. They cover
concurrent likes, committed writes, feed ordering, Unicode, JWT validation,
auth precedence, generic failures, and degraded health.

Validated locally: Release build succeeded with warnings treated as errors;
the official suite passed **42/42** checks, and `verify.py` passed.
The Ubuntu installation script was syntax-checked but has not been run on Ubuntu.

## Implementation choices

- Direct hosting removes the proxy hop and its CPU overhead on the single-vCPU
  benchmark server. Kestrel handles the client connections itself; capacity still
  needs to be measured with the repository's load test.
- Parameterized SQL queries read SQLite for each request, including current
  like counts. No query results, responses, or JWT verifications are cached.
- Reader connections are long-lived and kept in a small in-process pool, each with
  its statements prepared once. Microsoft.Data.Sqlite's own pooling would re-run the
  connection pragmas and re-prepare every statement on each request. A connection
  that hits an error is discarded, so `/health` still reports a broken database.
- `PRAGMA mmap_size` (512 MB, more than the ~250 MB seed) makes all connections read
  through the shared OS page cache rather than per-connection page caches.
- One dedicated writer connection (WAL, `synchronous=NORMAL`, foreign keys on) with
  prepared statements, behind an asynchronous semaphore so writers never block
  request threads. Autocommit completes before the response; `INSERT ... RETURNING`
  is exhausted before returning.
- A like is a single `INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`;
  only when nothing was inserted does a second query tell 200 apart from 404.
- Reads stay synchronous: Microsoft.Data.Sqlite has no real async I/O (its `*Async`
  methods run synchronously), so async calls would only add overhead.
- The feed uses the existing timestamp/ID index and count subquery. Timestamp
  strings come directly from SQLite, including its default on new posts.
- HS256 signatures are recomputed with the platform HMAC implementation for every
  authenticated request and compared in constant time. Expiry is mandatory and
  checked without clock skew. Token usernames are used as supplied on creation.
- Responses are written with a reused per-thread `Utf8JsonWriter` straight from the
  data reader (no intermediate objects or reflection-based serializer) and sent with
  `Content-Length`. Property order is explicit; manual request parsing enforces the
  specified errors and authentication-before-ID validation.
- Runtime settings for one vCPU: invariant globalization (ICU isn't loaded),
  non-concurrent GC (no background GC thread), and a minimum of 8 thread pool
  threads so a briefly blocked thread doesn't stall other requests.
- Routine request logging is disabled to limit console I/O; unexpected exceptions
  are logged and returned as the specified generic JSON error.

No load benchmark has been performed; conformance does not establish a user-capacity score.

References: [Kestrel endpoint configuration](https://learn.microsoft.com/en-us/aspnet/core/fundamentals/servers/kestrel/endpoints?view=aspnetcore-10.0),
[Kestrel hosting without a reverse proxy](https://learn.microsoft.com/en-us/aspnet/core/fundamentals/servers/kestrel/when-to-use-a-reverse-proxy?view=aspnetcore-10.0),
[Microsoft.Data.Sqlite 10.0.12](https://www.nuget.org/packages/Microsoft.Data.Sqlite/10.0.12),
[official .NET installation](https://learn.microsoft.com/en-us/dotnet/core/install/linux-scripted-manual).
