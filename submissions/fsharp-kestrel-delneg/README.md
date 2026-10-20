# F# on raw Kestrel (fsharp-kestrel-delneg)

| Item | Value |
|---|---|
| Language | F# 10 on .NET 10 (SDK 10.0.401, pinned in `global.json`) |
| Server | Kestrel (ASP.NET Core 10) with a connection handler. No web framework. |
| Database driver | SQLite 3.53.4 from source, called through `DllImport` (`Sqlite.fs`) |
| JSON | `System.Text.Json` (`Utf8JsonWriter`, `Utf8JsonReader`) |
| Build | ReadyToRun with TieredPGO. Workstation GC, not concurrent. |
| **Nginx or direct** | **Direct**: the server listens on `HOST:PORT` (`0.0.0.0:80`) itself |

## How it works

1. Kestrel accepts the connections. A connection handler (`Conn.fs`) receives the bytes of each connection. The handler does not call the next handler, so Kestrel's HTTP layer does not run.
2. The handler parses HTTP/1.1 itself. It finds the method, the path and three headers: `Authorization`, `Content-Length` and `Connection`. Routes are in code.
3. Each endpoint (`Api.fs`) writes the full response into the output pipe: the status line, the headers and the JSON body. The handler sends all the responses of one read with one flush.
4. One SQLite connection serves all requests. A lock serializes the requests. The server prepares each statement one time at start.
5. The JWT check (`Auth.fs`) runs on each request. It decodes the header and the payload, computes HMAC-SHA256 over the token bytes and compares the signature in fixed time. Then it checks `sub`, `username` and `exp`.

## Rules

- Writes: WAL with `synchronous=NORMAL`. Each write is an autocommit statement. The statement commits before the server sends the response.
- No cache: each request runs its SQL. The server keeps no copy of rows, responses or JWT results.
- `install.sh` (root) installs gcc, libicu and the .NET SDK from the official tarball with a SHA-512 check.
- `build.sh` (normal user) gets the SQLite source with a SHA-256 check, compiles it with gcc, and publishes the server.

## Optimizations

| Change | Effect on the local machine | Idea from |
|---|---|---|
| Own HTTP/1.1 parser in a Kestrel connection handler. One response write, one flush for each read. | CPU per request -9.6%. RSS 748 MB to 490 MB at 64,000 users. | #74 (own parser, one response buffer), #27 (raw Kestrel, no `HttpContext`) |
| `locking_mode=EXCLUSIVE`. SQLite takes the file lock one time, not around each statement. | CPU per request -8.4%. Hold p99 rose from 2 ms to 10 ms (checkpoints on the request thread). | #77, #27, #74 |
| SQLite from source with `THREADSAFE=2` and SQLite's recommended options | No mutex calls in SQLite | #77, #108 |
| `mmap_size` 1 GB, `cache_size` 32 MB | Reads use memory, not `read` calls | #108 |
| JSON from the SQLite UTF-8 bytes, no .NET strings for rows | Less allocation | #77 |
| HMAC on the token bytes with a reused `IncrementalHash` for each thread | No allocation for each request | #108 |
| Inline socket completions, inline scheduling in Kestrel | No thread switch for each request | #77, #108 |
| Workstation GC, not concurrent. ReadyToRun with TieredPGO. | Less CPU and memory on one core | #108 |

Changes that did not help (discarded):

| Change | Result | Idea from |
|---|---|---|
| `WaitForDataBeforeAllocatingBuffer=false` | Same CPU, RSS +237 MB | #77 |
| Own JSON writer | Same CPU | #74 |
| Native AOT | CPU +2%, RSS -93 MB | #77, #74 |
| SQLite with `-O3 -march=native` | Same CPU | #60 |
| `wal_autocheckpoint=8000` | CPU +4%, hold p99 72 ms | — |

## Local results (diagnostics, not an official score)

These numbers come from one local machine, not from the official box. Use them to compare versions on that machine only.

- Machine: WSL2 on a Ryzen 9 7900X3D.
- Server: Docker `ubuntu:24.04` with `--cpus=1 --memory=2g --memory-swap=2g`, one core, `nofile` 65535. It serves direct.
- Load: kload (#87) on other cores, with the request mix of `bench/load.js`.
- Run: 64,000 users, 60 s ramp, 120 s hold.

| Version | CPU per request | CPU in the hold | Hold p99 | Failed | Peak RSS |
|---|---|---|---|---|---|
| First version (Kestrel HTTP layer) | 68.09 µs | 46.1% | 3.7 ms | 0 | 748 MB |
| Own parser | 61.56 µs | 41.7% | 1.9 ms | 0 | 490 MB |
| `locking_mode=EXCLUSIVE` | 56.37 µs | 38.2% | 9.7 ms | 0 | 496 MB |

`test/test.sh`: 42/42.
