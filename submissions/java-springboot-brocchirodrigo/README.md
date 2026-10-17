# Java + Spring Boot 4 (WebFlux)

| | |
|---|---|
| Language | Java 26 (Eclipse Temurin 26.0.2.1, official tarball) |
| Framework | Spring Boot 4.1.1 (Spring Framework 7.0.9), WebFlux on Reactor Netty 1.3.7 / Netty 4.2.17 |
| JSON | Jackson 3.1.5, the one Spring Boot ships |
| SQLite driver | none: `libsqlite3` (Ubuntu's `libsqlite3-0`) called directly through the Java FFM API (`java.lang.foreign`) |
| Build | Maven 3.10.0 (official tarball) -> executable jar, `target/app.jar` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

No dependency besides `spring-boot-starter-webflux`: no JDBC driver, no JWT library, no validation library, no
Lombok. Versions are pinned by the Spring Boot parent POM.

## Running it

```bash
sudo bash install.sh   # apt: ca-certificates, curl, libsqlite3-0; JDK 26 to /opt/jdk-26, Maven to /opt/maven
bash build.sh          # mvn package -> target/app.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Layout

Clean Architecture, packages by layer:

```
com.brocchirodrigo.feed
├── domain             records (Post, PostBody, AuthenticatedUser), enums in model/enums, repository contracts,
│                      PersistenceException
├── application        use case interfaces and record implementations, input commands, sealed outputs,
│                      TokenVerifier port
├── adapter
│   ├── controller     functional WebFlux handlers (records), route table (FeedRoutes), request DTOs
│   └── presenter      response factory, error messages, response DTOs (records)
└── infrastructure
    ├── config         dependency wiring with @Bean methods, typed properties
    ├── persistence    SQLite through FFM, repositories, row mapper
    ├── security       HS256 verifier
    └── exception      global error handler
```

Domain and application code is plain Java with no framework annotations. Use cases and repositories receive
their dependencies through constructors (record components). They are wired in `infrastructure/config` by
`@Configuration(proxyBeanMethods = false)` classes with `@Bean` methods: no proxies, no `@Autowired`, no component
annotations on application code. The HTTP route table lives in the adapter (`FeedRoutes`), the config only
wires it.
Results are sealed types (`PostLookup`, `CreatePostResult`, `AuthResult`, `HealthStatus`) matched with exhaustive
`switch` and record patterns.

## Optimizations, and why

- **One event loop per core.** Reactor Netty starts at least 4 event loops. The app sets `ioWorkerCount` to the
  number of available processors, so the 1-vCPU box runs a single loop with no context switches between loops.
- **Handlers run inline on the event loop.** Every query is an in-process SQLite lookup of a few microseconds,
  so there is no hop to a blocking thread pool.
- **SQLite through FFM, not JDBC.** Downcalls go straight into `libsqlite3`. Short calls (bind, column, reset,
  changes) use `Linker.Option.critical`, which skips the thread-state transition. Statements are prepared once
  per connection with `SQLITE_PREPARE_PERSISTENT` and reused.
- **Each repository owns one connection and its prepared statements**, opened `NOMUTEX` and guarded by an
  uncontended lock (the box runs a single event loop). No `ThreadLocal`; repositories finalize their statements
  and close their connection on shutdown.
- **Column text read with its known length.** SQLite reports the byte length, so text is copied once into a
  reused buffer and decoded, instead of scanning for the terminator and allocating an intermediate array.
- **Ids parsed in one pass.** Post ids and token subjects are validated and converted in the same loop, with no
  allocation and no exception (`0` means invalid, ids are positive).
- **Errors are values.** Expected failures (bad token, missing post, invalid body) are sealed results, not
  exceptions, so nothing allocates a stack trace on the request path.
- **Functional routes** (`RouterFunction`) instead of annotated controllers: no per-request argument resolution,
  and auth is checked before the id because each handler decides its own order.
- **JWT verified by hand on every request** (no caching, rule 5): HS256 only, a JDK `Mac` keyed once with
  reused input, digest and signature buffers (no allocation on the HMAC path), constant-time signature compare,
  `exp`/`nbf` checked. The canonical `{"alg":"HS256","typ":"JWT"}` header is matched in place, so only other
  headers get parsed.
- **Direct instead of Nginx.** On one core, Nginx's CPU would come out of the app's budget. Netty keeps idle
  keep-alive connections open with no idle timeout and no connection cap.
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 64 MiB page cache,
  `temp_store=MEMORY`. Autocommit commits each write before its response is sent.
- **JVM sized for the 2 GB box.** Fixed 768 MB heap (`-Xms` = `-Xmx`, no resizing under load) with a 512 MB young
  generation: almost everything a request allocates dies young, and Serial GC's young collections cost what
  survives, not the heap size, so a large young generation means fewer collections. The rest of the box covers
  the OS (~300 MB), JVM non-heap (~250 MB), SQLite caches and the database's page cache, with no swap. Serial GC
  (one vCPU), compact object headers (JEP 519), native access enabled for FFM.

## License

MIT, under the repo's [license](../../LICENSE).
