# C# on raw io_uring, Native AOT

A single-threaded HTTP/1.1 server that calls Linux io_uring and the SQLite C API directly. It is
compiled ahead of time to a standalone binary. The same server built for the .NET JIT is
[`csharp-iouring-jit-gichamba`](../csharp-iouring-jit-gichamba/).

| | |
|---|---|
| Language | C#, .NET SDK 10.0.401 (`net10.0`) |
| Runtime | Native AOT, x86-64-v3 (AVX2) |
| Framework | None: HTTP/1.1 on io_uring, Linux 6.1 or newer |
| SQLite | 3.53.4, built from source and linked statically |
| Driver | None: the SQLite C API is called directly |
| Packages | None |
| Nginx or direct | Direct, on `HOST:PORT` |

## Build and run

```bash
sudo bash install.sh   # gcc, clang, the libraries .NET needs, and the .NET SDK (SHA-512 checked)
bash build.sh          # SQLite with GCC PGO, then the Native AOT binary in bin/feed
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` waits for apt's lock, retries downloads, and keeps the SDK in its own directory,
so running it twice or on a box with another SDK is safe. `build.sh` checks SQLite's SHA-256 and
restores in locked mode. On a CPU without AVX2 it targets x86-64-v2 instead. The server passes
all 42 checks in `test/run.sh`.

## How it works

- One thread does everything: accept, receive, parse, query and send. The box has one core,
  so more threads only add context switches and locks. SQLite is compiled without mutexes.
- Connections do not use file descriptors. Sockets are accepted into io_uring's registered
  file tables: up to 8 rings of 65,535 slots. `LimitNOFILE=65535` no longer caps connections,
  which is what stopped the first version of this entry (#74) at 64,000 users.
- Each pass of the event loop is one batch. All complete requests in it run in a single SQLite
  transaction, which commits once, and only then do the responses go out. Many likes and posts
  share one commit, and every 201 follows its commit (rule 6). Reads in a batch see the batch's own
  earlier writes. But nothing is sent before the commit, and if the commit fails every response in
  the batch becomes a 500, so no client ever sees uncommitted data.
- The kernel batches completion work (`DEFER_TASKRUN`). After a busy pass the loop waits for up
  to 128 completions or 4 ms, so a burst is handled in one pass.
- SQLite: WAL, `synchronous=NORMAL`, exclusive locking (no file lock per transaction), a
  passive checkpoint every 1,024 WAL frames run between passes instead of inside COMMIT, 1 GiB
  `mmap_size`. Every statement is prepared once. The feed comes from three cheap range scans when
  the newest 20 posts have close ids, and from the plain join otherwise.
- Responses are written straight into reused buffers, and SQLite text is copied as bytes
  without becoming .NET strings. The JWT is verified on every request (rule 5), `exp` is required,
  and only HS256 is accepted.

## Measurements

These are our rigs, not official scores. Rust #82 ran on the same rigs as the reference.

- Azure F1s (1 vCPU, 2 GB, Ubuntu 24.04, kernel 6.8, real disk, separate load machines), three
  five-minute holds at 120,000 users each, zero failures. Median p99 136 ms against 291 ms for Rust
  #82. The whole VM was 54–55% busy against 64–65% for Rust. All entries, Rust included, failed at
  135,000 users there, which points to that VM's network path rather than the servers.
- One-box loopback rig (server in a 1-vCPU, 2 GB container): five-minute holds passed at
  287,000 users for this build, and at 292,000 for both the JIT build and Rust #82. Repeated holds
  near that load passed and failed for all three, so the entries tie within noise.

## Trade-offs

This code serves one small, fixed API on a one-core box. It is not a general web server. The HTTP
parser handles what the API needs and closes the connection on anything else: chunked bodies,
whitespace before a header colon, conflicting `Content-Length` headers. It runs only on Linux
x86-64. Because one thread does everything, a slow query would stall every request; every query
here is an index lookup.

## Credit

- The request validation rules and the JWT verifier are adapted from the C# entry by maks-march
  (#27, MIT).
- The range-scan feed and the GCC profile-guided SQLite build follow Scooter1337's Rust entry (#82).
- Deferred task work, waiting for 128 completions or 4 ms after a busy pass, and the fallback to
  classic provided buffers on kernels that refuse a buffer ring follow raoz's C entry (#26).

## License

MIT, under the repository's license.
