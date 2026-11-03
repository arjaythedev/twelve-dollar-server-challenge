# C# on raw io_uring, .NET JIT

The server from [`csharp-epoll-gichamba`](../csharp-epoll-gichamba/), built for the .NET JIT instead
of Native AOT. The C# source is the same. Only the build and the way SQLite is linked differ.
That README describes the design.

| | |
|---|---|
| Language | C#, .NET SDK 10.0.401 (`net10.0`) |
| Runtime | .NET 10 JIT, framework-dependent |
| Framework | None: HTTP/1.1 on io_uring, Linux 6.1 or newer |
| SQLite | 3.53.4, built from source as a shared library next to the app |
| Driver | None: the SQLite C API is called directly |
| Packages | None |
| Nginx or direct | Direct, on `HOST:PORT` |

## Build and run

```bash
sudo bash install.sh   # gcc and the .NET SDK (SHA-512 checked), which also runs the server
bash build.sh          # SQLite with GCC PGO as bin/libsqlite3.so, then the app in bin/
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`install.sh` waits for apt's lock, retries downloads, and keeps the SDK in its own directory, so
running it twice or on a box with another SDK is safe. The server passes all 42 checks in
`test/run.sh`.

## Why a JIT entry

The JIT compiles with the exact CPU's instruction set and recompiles hot methods with profile
data while it runs. Native AOT gives a standalone binary and no warm-up. On our rigs neither mode
won.

- Azure F1s (1 vCPU, 2 GB, real disk, separate load machines), three five-minute holds at
  120,000 users: median p99 135 ms against 136 ms for AOT, with overlapping ranges of app CPU per
  request.
- One-box loopback rig: five-minute holds passed at 292,000 users for this build and for Rust
  #82, and at 287,000 for AOT. Repeated holds near that load passed and failed for all three.

Until the JIT has recompiled the hot paths, the server runs slower code. In a 90-second probe at
240,000 users, p99 was 195 ms against 24 ms for AOT, which fits that warm-up.

## Credit

As for the AOT entry: validation rules and the JWT verifier adapted from maks-march's C# entry
(#27, MIT), the range-scan feed and GCC-PGO SQLite from Scooter1337's Rust entry (#82), and the
deferred task work, completion batching and buffer fallback from raoz's C entry (#26).

## License

MIT, under the repository's license.
