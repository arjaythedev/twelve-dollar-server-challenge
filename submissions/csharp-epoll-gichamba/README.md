# C# on a hand-rolled epoll server

This entry skips ASP.NET Core and talks to Linux and SQLite directly. It compiles to one Native
AOT binary and serves HTTP itself, with no Nginx in front.

It started as a simple bet: the fastest C# entry so far (maks-march's raw Kestrel server, PR #27,
32,000 users in the video) would get a lot faster with Native AOT. It didn't. On the same box, AOT
alone scored 7.6% lower than the JIT. The time was going into the framework and the thread pool,
not into the compiler, so this entry replaces both.

| | |
|---|---|
| Language | C#, .NET SDK 10.0.401 (pinned in `install.sh`), `net10.0` |
| Runtime | Native AOT, one binary, built for the box's own CPU |
| Framework | None: a single-threaded HTTP/1.1 server on epoll |
| SQLite | 3.53.4 (`sqlite-autoconf-3530400`), built from source and linked into the binary |
| Driver | None: the code calls the SQLite C API directly |
| NuGet packages | None |
| Nginx or direct | Direct, on `HOST:PORT` (`0.0.0.0:80` on the benchmark box) |

## Build and run

```bash
sudo bash install.sh   # .NET SDK 10.0.401 tarball, plus clang, binutils, zlib1g-dev
bash build.sh          # builds SQLite, then dotnet publish -> bin/feed
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`build.sh` downloads `sqlite-autoconf-3530400` from sqlite.org, checks its SHA-256, and compiles
`sqlite3.c` with clang (`-O2 -march=native`, `-DSQLITE_THREADSAFE=0` and the other flags in the
script). The publish step restores in locked mode, so it follows `packages.lock.json`.

`SQLITE_PATH` and `JWT_SECRET` are required. `HOST` defaults to `127.0.0.1` and `PORT` to `3000`.
`test/run.sh` passes all 42 checks on Ubuntu 24.04.

## How it works

One thread does everything: accept, read, parse, query and write. The box has one core, so extra
threads only add context switches and locks. On the same box, PR #27's code (built with AOT)
caused about 50,000 context switches a second at 52,000 users; this one causes about 4,300 at
64,000 users. SQLite is compiled
without mutexes because only that one thread ever touches it.

Each wake-up from `epoll_wait` is handled as one batch. Every complete request in the batch runs
inside a single SQLite transaction, the batch commits once, and only then do the responses go out.
That gives group commit for free: under load, dozens of likes and posts share one commit, and every
201 is sent after its row is committed (rule 6). If the commit fails, or SQLite rolls the batch back
partway, every response in that batch becomes a 500 and the rest of the batch does not run.

The database runs in WAL mode with `synchronous=NORMAL` and exclusive locking, which keeps the WAL
index in memory and skips file locking on every transaction. Checkpoints are moved out of COMMIT
(`wal_autocheckpoint` is off): a WAL hook records the WAL size, and a passive checkpoint runs at 1,000 frames, after that batch's
responses have been sent. `mmap_size` is 1 GiB and `cache_size` is 16 MiB.

Every statement is prepared once at startup. A like is a single
`INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`, and the "does this post exist"
query only runs when nothing was inserted.

Responses are written straight into one reused byte buffer: status line, headers and JSON. Column
text is copied from SQLite as bytes without becoming a .NET string, and only `"`, `\` and control
characters are escaped. The JWT is verified on every request (rule 5); only the HMAC key object is
reused. Idle connections hold no buffers at all, and the GC is the workstation, non-concurrent
kind, so there is no background GC thread competing for the one core.

## What I measured

These are not official scores. They come from one Azure Standard_F1s (1 dedicated vCPU, 2 GB, no
swap, Ubuntu 24.04) with the database on the VM's local SSD. k6 1.3.0 ran the unchanged
`bench/load.js` from a separate VM in the same region, using the README's procedure: a warm-up,
doubling from 2,500 users, narrowing to within 250, then a 5-minute hold. All four entries ran on
the same box, one after another.

| Entry | Users held | p99 at that load | Server CPU at that load |
|---|---:|---:|---:|
| PR #27, C# on raw Kestrel | 56,000 | 615 ms | not recorded |
| PR #27 built with Native AOT | 51,750 | 680 ms | 91% at 52,000 |
| This entry | 64,000 | 53 ms | 39% |
| PR #19, Rust on io_uring | 64,000 | 46 ms | 29% |

64,000 is a ceiling, not this entry's limit. The kernel's connection-tracking table holds 65,536
entries, and it was 97% full at 64,000 users, the same wall the video hit. Rust gets there with
less CPU, most likely because io_uring saves system calls that epoll still makes.

The Azure box is faster than the droplet in the video (PR #27 held 56,000 here against 32,000
there), most likely because its vCPU is a dedicated core rather than a shared one. Compare the rows
with each other, not with the video.

## Trade-offs

This is code for one small, fixed API on a one-core box, not a pattern for a general web service.
The HTTP parser handles what this API needs and is strict about the rest: it closes the connection
on chunked bodies, on whitespace before a header colon, and on conflicting `Content-Length`
headers. It runs only on Linux x86-64. Because one thread does everything, a slow query would
stall every request; here every query is an index lookup that takes well under a millisecond.

## Credit

The request validation rules and the JWT verifier are adapted from the C# entry by maks-march
(PR #27, MIT). Starting from code that already passed the tests made the behaviour easy to match.

## License

MIT, under the repo's [license](../../LICENSE).
