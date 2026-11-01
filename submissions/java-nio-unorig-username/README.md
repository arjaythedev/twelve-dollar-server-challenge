# Java + java.nio + SQLite (under protest)

| | |
|---|---|
| Language | Java 25 (Eclipse Temurin 25.0.4.1+1 LTS, official tarball, SHA-256 pinned in `install.sh`), `javac`, HotSpot C2 |
| Framework | none: one `java.nio` `Selector` loop (epoll), hand-written HTTP/1.1, JSON and JWT code. No Spring. No Netty. No beans of any kind. |
| SQLite | 3.54.1, the official amalgamation (SHA-256 pinned in `build.sh`), built with gcc into `bin/libsqlite3.so` and called through `java.lang.foreign` (FFM) |
| Other dependencies | none (HMAC-SHA256 is the JDK's `javax.crypto.Mac`; base64url and JSON are in `Server.java`). No JDBC, no xerial, no prebuilt natives. |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## A word from the author

I was asked to write this in Java. I said no. I was asked again, nicely, and I said a word that rhymes
with "duck snow". Then I did it anyway, because apparently I have no spine, only a classpath.

This is the C submission's design, ported line by line into a language that thinks `throws Throwable`
is a reasonable thing to put on a method signature. It has no `AbstractSingletonProxyFactoryBean`, no
`@Autowired`, no `applicationContext.xml`, and no 400 MB fat jar. It is one file. It compiles with one
`javac` call. If that makes you uncomfortable, there is a Spring Initializr tab open somewhere waiting
for you, and it misses you.

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip; Temurin JDK 25 into /opt/temurin-25
bash build.sh          # downloads and verifies SQLite, builds bin/libsqlite3.so and bin/Server.class
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

JVM flags (`start.sh`): `-XX:+UseSerialGC` (one vCPU: a parallel collector would just fight itself),
`-Xms256m -Xmx512m -Xmn64m`, `-XX:ActiveProcessorCount=1`, `-XX:-UsePerfData`. It is healthy a second
or two after start, which by Java standards is basically instant and should be studied by scientists.

## Design

- **One process, one thread, one `Selector` loop.** The box has one vCPU, so threads would only add
  context switches. Every query is an indexed lookup that takes microseconds, so SQLite is called
  inline on the loop through FFM downcalls. Pointers travel as plain `long`s and the small calls
  (`bind`, `column_*`, `reset`) are linked as critical, so a column read costs a call, not a ceremony.
- **No allocation per request on the hot path.** Reads go into one direct `ByteBuffer`, are parsed out
  of static `byte[]` buffers, and responses are built in one static `byte[]` arena that is copied once
  per batch into a direct buffer and written from there. Parser state is static. The garbage collector
  has almost nothing to do, which is the only relationship with a garbage collector that works.
- **A transaction per loop iteration (group commit).** The first request in a `select` batch that
  touches the database opens a deferred `BEGIN`. Every read and write in the batch runs inside it, and
  the batch ends with one `COMMIT`. Responses are written into the arena while the batch runs and are
  only sent after that `COMMIT` returns, so a `201` always means the row is committed (rule 6), and a
  read never reports data that could still be rolled back. If a commit fails, nothing from that batch
  is sent: those connections are closed instead.
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same class, started
  as a second JVM with `--ckpt` and a 24 MB heap) runs `PASSIVE` checkpoints in a loop. The event loop
  never waits on the disk, and the WAL stays a few MB under normal load. If the helper can't keep up
  (the CPU is pegged for minutes), the server's own `wal_autocheckpoint=65536` still caps the WAL at
  about 256 MB. Durability is unchanged: WAL with `synchronous=NORMAL`, and a response waits for its
  `COMMIT`. Yes, it is an entire JVM whose job is to call one C function in a loop. I'm aware.
- **SQLite settings**: WAL, `synchronous=NORMAL`, normal locking, 1 GiB `mmap_size`, a 16 MB page cache,
  and prepared statements built once with `SQLITE_PREPARE_PERSISTENT`. SQLite is compiled
  single-threaded (`SQLITE_THREADSAFE=0`, only the loop thread ever touches it) with unused features
  left out. Queries are the reference queries; a like is one statement,
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`, with a post lookup only when
  nothing was inserted (to tell "already liked" from "no such post").
- **No caching** (rule 5): every request runs its query, and every token's HMAC is computed
  (`javax.crypto.Mac`, keyed once from `JWT_SECRET` at startup).
- **Past 65,535 clients.** Serving direct, every k6 user holds a keep-alive connection, and
  `LimitNOFILE=65535` caps descriptors per process. When this process gets within about 4,000
  descriptors of that cap, it starts another JVM of itself with `--shared` on the same port
  (`SO_REUSEPORT`), waits for the new one to print `1` on its stdout (it is listening), stops accepting,
  and resumes accepting when it has room again. The copy does the same if it fills up. Below the cap
  there is exactly one server process. Workers share the database through WAL; a write that finds its
  read snapshot outdated by another worker's commit restarts its transaction with `BEGIN IMMEDIATE`.
  Workers and the checkpointer call `prctl(PR_SET_PDEATHSIG, SIGTERM)` through FFM, so they die with
  their parent (the JDK has no API for this, naturally).
- **Strict input handling**: a full RFC 8259 JSON validator (duplicate keys: last wins, like
  `JSON.parse`), `\u` escapes and surrogate pairs decoded, invalid UTF-8 rejected as malformed JSON,
  whitespace trimmed with JavaScript's `String.prototype.trim` set, and the 500-character limit counted
  in Unicode code points (what SQLite's `length()` counts in the schema's `CHECK`). JWTs: HS256 only,
  constant-time signature compare, `exp` and `nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0, and
  `Expect: 100-continue`. Chunked request bodies aren't supported (they're out of scope).
- **No tuning outside the process** (rule 11): the JVM raises its own soft descriptor limit to the hard
  limit it was given (`-XX:+MaxFDLimit`, on by default), and that's it.

## Things you will not find in this repo

- `pom.xml`, `build.gradle`, `settings.gradle.kts`, `mvnw`, or a `.mvn/` folder full of regret
- a `src/main/java/com/example/feed/api/v1/controller/impl/` directory
- `FeedServiceFactoryProviderBeanPostProcessorAdapter`
- a log4j version you have to explain to security
- any code that runs because of an annotation you didn't write

## Local measurements

See the numbers in the pull request description. They were measured with the server container limited
to one CPU, 2 GB RAM, no swap and 65,535 descriptors, with load generators in separate containers.

## License

MIT, under the repo's [license](../../LICENSE).
