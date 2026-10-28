# Java 25 + JDK HttpServer

| Component | Version / choice |
|---|---|
| Language | Java 25; installer pins Eclipse Temurin 25.0.4.1+1 |
| HTTP | JDK `com.sun.net.httpserver.HttpServer` (`jdk.httpserver`) |
| SQLite | SQLite 3.53.4 via JDK 25 FFM, checksum-pinned native build with `SQLITE_THREADSAFE=2` |
| JSON | Jackson Core 2.22.4 streaming parser/generator |
| JWT | JDK HmacSHA256, Base64 URL decoder and constant-time signature comparison |
| Build | Apache Maven 3.9.11; dependency and plugin versions pinned in `pom.xml` |
| Nginx or direct | **Direct**, no proxy required |

## Build and run

On a clean Ubuntu 24.04 x86_64 machine:

```bash
sudo bash submissions/java-jdkhttpserver-razorree/install.sh
bash submissions/java-jdkhttpserver-razorree/build.sh
```

`install.sh` installs a checksum-verified Temurin distribution under
`/opt/java-jdkhttpserver`, without changing the system Java alternative.
Build and startup prefer an existing Java 25 via `JAVA_HOME`, then `PATH`,
then the pinned installation. Java 25 is required; no preview features are used.
The build downloads checksum-verified Maven into the ignored `.tools/` directory,
uses a local Maven repository there, and produces `target/server.jar` plus
`target/lib/` and `target/native/`. It needs curl, tar, Python 3, GCC/libc headers,
and network access on the first build. `build-native.py` verifies source and cached
library hashes and records compiler, flags, native version and compile options in
`target/native/manifest.json`. The native build uses `-O2 -fPIC -shared` and
`-DSQLITE_THREADSAFE=2`; it opens the single Java-locked connection with `FULLMUTEX`.

From the repository root, create the deterministic seed once if needed, then
start from a copy (never serve `seed/feed.db` directly):

```bash
bash seed/make-seed.sh
mkdir -p results
cp seed/feed.db results/java-feed.db
SQLITE_PATH="$PWD/results/java-feed.db" \
JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 \
bash submissions/java-jdkhttpserver-razorree/start.sh
```

The first command requires Node >=18 and the sqlite3 CLI. Skip it if the seed
already exists. For the official direct benchmark, set `HOST=0.0.0.0 PORT=80`;
the process must have permission to bind port 80. The benchmark supplies the
database path and JWT secret. `SQLITE_PATH` must reference an existing
database and `JWT_SECRET` must be nonempty. HOST/PORT default to 127.0.0.1:3000.

To restrict the process to one logical CPU for a local comparison:

```bash
SQLITE_PATH="$PWD/results/java-feed.db" \
JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 \
taskset -c 0 bash submissions/java-jdkhttpserver-razorree/start.sh
```

`start.sh` executes Java in the foreground with the accepted benchmark settings:
an initial pre-touched 512 MiB heap, a 1,024 MiB maximum heap, a 448 MiB young generation, Serial GC, compact object headers, virtual
thread scheduler parallelism 2, FFM native access, `MALLOC_ARENA_MAX=1`, and
`-XX:ActiveProcessorCount=1`. That processor-count flag is a JVM hint, not a CPU
or total-memory limit; enforce the one-core, 2 GiB service budget externally.

## Implementation choices

- Virtual threads handle HTTP requests, with at most 1,024 handlers admitted at once.
  A `ReentrantLock` serializes access to
  one SQLite connection and its reusable prepared statements. SQLite read results
  are escaped into JSON while each row is valid; response writes release the lock.
- WAL, `synchronous=NORMAL`, foreign keys, five-second busy timeout, 64 MiB
  SQLite page cache, 1 GiB mmap address range and memory-backed temporary data.
  `mmap_size` is an address-space allowance, not a 1 GiB allocation.
- Explicit write transactions commit before a success response is sent.
  Exceptions roll back. Result sets, including `INSERT ... RETURNING`, are
  exhausted/closed before commit. Failed foreign-key writes cannot poison the
  next transaction. One writer drains up to 256 immediately available requests per
  transaction with per-request savepoints; no artificial batching delay. No ORM
  or connection pool.
- Every feed and post request reads SQLite. JWT signatures and time claims are verified on
  every authenticated request. Request JSON is parsed strictly; generated JSON
  uses the spec's field order. SQLite read responses copy and escape UTF-8 bytes
  without constructing post records or text Strings. Body trimming uses the reference whitespace set,
  and body length counts Unicode code points rather than UTF-16 code units.
- HttpServer's default 200 idle connections and 30-second idle timeout are
  increased to 8,192 and 75 seconds. Open connections are not otherwise capped
  by HttpServer. TCP_NODELAY is enabled; the requested accept backlog is 4,096.
  The OS file-descriptor limit and memory still constrain actual connections.
- Request bodies larger than 16 KiB return 413; they are outside the spec's
  required behavior. Exchanges are closed after response completion, and
  HttpServer manages keep-alive and draining unread request bodies. Requests
  with absent or zero content length and no transfer encoding observe EOF before
  routing, avoiding a drain buffer for these empty bodies.

## Validation and benchmarking

The official CI command builds and tests against a fresh database copy:

```bash
PORT=3002 bash test/run.sh submissions/java-jdkhttpserver-razorree
```

Additional checks require Python 3 (standard library only), Linux `taskset`,
and an already built submission. They manage their own temporary database and
Java processes and include a 66-second same-socket idle check with 256 connections:

```bash
python3 submissions/java-jdkhttpserver-razorree/tests/check.py --cpu 0
python3 submissions/java-jdkhttpserver-razorree/tests/check-admission-shutdown.py
```

Optionally append `--k6 /path/to/k6` to run a 1,000-VU, 30-second smoke hold on a
fresh database after those checks. The generator excludes the server CPU and
its SMT siblings. Results, process logs, CPU affinity and sampled peak RSS go
to a new `results/java-jdkhttpserver-<UTC timestamp>/` directory, or `--output PATH`.
That smoke test is not a full capacity score.

For the full challenge procedure, start the server on a fresh database copy and
use the repository's capacity runner, ideally with k6 on a separate machine:

```bash
python3 bench/find-capacity.py --base-url http://SERVER:3000
```

For local runs, keep k6 off the server CPU and its SMT sibling. This submission
reached 250,250 VUs in a local two-minute hold following lower-load runs, but a
fresh-start run at that load failed. This is not an official five-minute score.

MIT, under the repository's license.

## Native binding lifecycle

The single connection retains FULLMUTEX in addition to Java exclusivity. Normal
FFM downcalls cover I/O and allocations; only short scalar operations are critical.
Text columns are copied using byte lengths before reset. Read responses escape
the copied UTF-8 bytes directly into JSON.
Shutdown joins the writer, finalizes every prepared statement, closes the database,
then shuts down and unloads SQLite when the last native connection closes.
Transaction fault tests use an external Python observer to avoid linking two
independent SQLite libraries into the test process.
