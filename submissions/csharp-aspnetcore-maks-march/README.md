# C# + ASP.NET Core (raw Kestrel)

| | |
|---|---|
| Language | C# (net10.0), .NET SDK **10.0.401** — LTS, pinned in `install.sh` (includes the ASP.NET Core 10.0.12 runtime) |
| Framework | **ASP.NET Core — Kestrel**, hosted directly through `IServer.StartAsync(IHttpApplication<TContext>)`: no `WebApplication` pipeline, no middleware, no routing, no `HttpContext` |
| Server | Kestrel, HTTP/1.1 only, keep-alive (default 130 s idle timeout) |
| SQLite driver | **Microsoft.Data.Sqlite 10.0.12** over SQLitePCLRaw `bundle_e_sqlite3`, a single long-lived connection guarded by a lock |
| JSON | `System.Text.Json` `Utf8JsonWriter` into pooled buffers; exact key order, compact output |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80` on the benchmark box) itself |

All NuGet versions, transitive ones included, are pinned in `packages.lock.json`
(`dotnet publish -p:RestoreLockedMode=true` refuses to deviate from it).

## Running it

```bash
sudo bash install.sh   # official .NET 10 SDK tarball -> /usr/share/dotnet, symlink /usr/local/bin/dotnet
bash build.sh          # dotnet publish -c Release -> bin/
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`test/run.sh` passes 42/42 on a fresh copy of the seed database.

## How it works

Kestrel parses HTTP and hands each request to `FeedApp.CreateContext/ProcessRequestAsync`
as raw `IFeatureCollection` features. The app matches method+path by hand (string
comparisons, no route table), validates ids and auth, runs one SQLite query per request
through prepared, reused commands, and writes the response as bytes straight to the
response stream with `Content-Length` set. Request contexts, JSON buffers and body buffers
are pooled, so steady-state read traffic allocates only what SQLite returns.

## Optimizations, and why

- **Raw `IHttpApplication<TContext>` instead of the middleware pipeline.** Every ASP.NET
  request normally pays for `HostingApplication.Context`, `DefaultHttpContext`, routing and
  endpoint selection. This app has five endpoints; matching them with string comparisons
  removes that entire layer. The same reason it goes **direct instead of Nginx**: on one
  core, Nginx's CPU comes straight out of the app's budget, and Kestrel already is a
  production HTTP server that comfortably holds thousands of idle keep-alive connections.
- **One connection + one lock.** The box has a single core and every query is an index or
  primary-key lookup finishing in well under a millisecond, so serializing on the
  connection costs nothing in throughput, makes `SqliteConnection` (not thread-safe) safe,
  and makes SQLite write-lock contention (`SQLITE_BUSY`) impossible. Commands are prepared
  once and reused — only parameter values change.
- **Zero-allocation hot path.** Pooled request contexts, pooled `IBufferWriter<byte>` JSON
  buffers reused via `Utf8JsonWriter.Reset`, `ArrayPool` request-body buffers, prebuilt
  byte arrays for every error response. GC (workstation, concurrent) almost never runs.
- **JWT verified by hand on every authenticated request** (rule 5): stackalloc buffers,
  a hand-rolled base64url decoder, a `ThreadStatic` pre-keyed `HMACSHA256`,
  `Utf8JsonReader` scans of header/payload, constant-time signature comparison. HS256
  only, `exp`/`nbf` checked, `sub`/`username` payload validation exactly per the spec
  (auth is checked before the id on `POST /posts/:id/like`).
- **SQL**: the reference queries. A like is one statement —
  `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING` — and the post-existence
  check runs only when nothing was inserted, to tell "already liked" from "no such post".
- **Pragmas**: `journal_mode=WAL` + `synchronous=NORMAL` (rule 6: every 201 is sent only
  after its row is committed — the `RETURNING` insert is stepped to `DONE` before the
  response), `mmap_size=1 GiB` (reads via the OS page cache), `cache_size=64 MiB`,
  `temp_store=MEMORY`, `busy_timeout=5000`, `foreign_keys=OFF` (the reference setup —
  a like of a missing post is a 404, not an FK error).
- **Trimming/validation identical to the reference**: bodies are trimmed with
  JavaScript's exact `String.prototype.trim()` whitespace set; ids are positive plain
  decimal integers, with values above SQLite's INTEGER range answered as 404 (a row that
  big cannot exist).
- **Runtime knobs**: workstation concurrent GC (tiny allocation rate, one core — shorter
  pauses than server GC), `ThreadPool.SetMinThreads(4)` to absorb arrival bursts without
  waiting for thread injection, `InvariantGlobalization`, HTTP/1.1-only listeners.

## License

MIT, under the repo's [license](../../LICENSE).
