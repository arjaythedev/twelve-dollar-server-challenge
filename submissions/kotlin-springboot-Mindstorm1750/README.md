# Kotlin + Spring Boot

How fast can a *conventional* Spring Boot app go on one vCPU? This one is written the way a Spring team would
write it: `@RestController`s, a service, a `JdbcClient` repository, a `@RestControllerAdvice` for errors and
kotlinx.serialization data classes. Every optimization below was kept only if a load test showed it winning.

| | |
|---|---|
| Language | Kotlin 2.3.21 |
| Framework | Spring Boot 4.1.1 (Spring MVC 7.0.9, Tomcat 11.0.24, virtual threads) |
| Runtime | Oracle GraalVM for JDK 25.0.4 on the JVM (Graal JIT), Serial GC, fixed 1 GB heap, transparent huge pages |
| SQLite driver | `org.xerial:sqlite-jdbc` 3.53.2.1 (bundled SQLite), HikariCP 7.0.2, Spring `JdbcClient` |
| JSON | kotlinx.serialization-json 1.11.0 (Jackson excluded) |
| Build | Gradle 9.8.1 wrapper (`bootJar`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All versions are pinned in `build.gradle.kts` (Spring Boot's BOM pins the transitive ones) and `install.sh`.

## Running it

    sudo bash install.sh   # Oracle GraalVM 25.0.4 (sha256-checked) into /opt/graalvm
    bash build.sh          # executable jar, ~40 s on 1 vCPU
    SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh   # healthy in ~5 s

## Layout

    auth/    JwtVerifier (HS256), AuthUser, the argument resolver that supplies AuthUser to controllers
    post/    PostController -> PostService -> PostRepository (SQL), DTOs, PostId/PostBody parsing
    health/  GET /health
    error/   ApiException types + one @RestControllerAdvice producing {"error": "..."}
    config/  the two SQLite pools, the statement cache, Tomcat buffers, MVC config

## Optimizations, and why

Measured with `bench/load.js` (see "How this was measured"). "CPU/request" is the container's CPU time divided
by requests served, the number that decides how many users one core can take.

- **Graal JIT, Serial GC, fixed 1 GB heap** (`start.sh`). With a 512 MB heap, GC and cache churn made the server
  unstable at high load; at 1 GB the Graal JIT beat C2 by 6-20% CPU/request in two orderings. Parallel and G1 GC
  add GC threads that compete with requests on one core. `-XX:+UseTransparentHugePages` maps the heap and code
  with 2 MB pages (Ubuntu's default THP mode is `madvise`, so nothing on the box is tuned): ~2.5% less
  CPU/request, confirmed on two different boxes.
- **No kotlin-reflect.** When it is on the classpath, Spring MVC calls every Kotlin handler through
  `KFunction.callBy` and runs kotlin-reflect lookups for every kotlinx body; its metadata caches are soft
  references that a busy heap keeps clearing. Without it, handlers are a plain `Method.invoke`: about 5% less
  CPU/request and half the p99. Two small costs: `FeedProperties` binds through setters (Boot's Kotlin
  constructor binding needs kotlin-reflect), and error bodies are serialized directly, because Spring Framework
  7.0.9 applies its kotlin-reflect-based `KotlinResponseBodyAdvice` to `@ExceptionHandler` results even when
  kotlin-reflect is absent.
- **Reused prepared statements** (`StatementCachingDataSource`, under Hikari). Profiles showed SQLite's
  `prepare` at about 40% of all SQLite time: every request re-parsed and re-planned its SQL. A small per-connection
  LRU of compiled statements removes that. It caches query plans, never data, holds no read snapshot between
  uses, and never evicts a statement in use (all covered by tests).
- **Filters this API doesn't need are off**: form-content and character-encoding filters, and Tomcat's WebSocket
  filter (its jar is excluded). With the statement cache: 227 -> 204 us CPU/request, p99 41 -> 22 ms.
- **Direct instead of Nginx**, and connections made cheap: every user holds a keep-alive connection, so Tomcat's
  per-connection buffers are 2 KB read / 4 KB write instead of 8 KB + 8 KB (an idle connection costs ~7.5 KB
  instead of ~17.5 KB), `max-connections=60000`, 75 s keep-alive, unlimited requests per connection.
- **Virtual threads.** A 16-thread platform pool used 29% more CPU/request and failed the latency thresholds at
  the same load.
- **Two SQLite pools**: 4 read-only readers and exactly one writer, so writes queue in the JVM instead of hitting
  `SQLITE_BUSY`. WAL + `synchronous=NORMAL` (rule 6), autocommit (each write commits before the response),
  512 MB `mmap_size` so readers share the OS page cache, and no `SELECT last_insert_rowid()` after inserts
  (posts come back through `RETURNING`).
- **One SQL statement per request.** A like is a single `INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`;
  only when it inserts nothing does a second query tell "already liked" from "no such post".
- **JWT verified by hand** (~60 lines, `javax.crypto.Mac`, constant-time compare, HS256 only, `exp` checked) instead
  of the Spring Security filter chain, on every request (rule 5). Under 1% of request time in profiles.

## What I tried and didn't keep

| Tried | Result |
|---|---|
| GraalVM native image | Can't be built on the box: even without kotlin-reflect, and with Gradle exiting first, `native-image` ran out of a 1550 MB build heap (it needs ~1.8 GB live heap for analysis; the box has 2 GB total, no swap). Built elsewhere for comparison: starts in 0.05 s at 97 MiB, but uses ~15% more CPU/request than the warmed Graal JIT. Did not try PGO. |
| Jetty instead of Tomcat | Cheaper idle connections, but collapsed under load (82% errors at 30k users where Tomcat passed). |
| C2 instead of the Graal JIT | Won only while the heap was too small; with 1 GB, 6-20% more CPU/request. |
| Parallel GC / G1 | Extra GC threads on one core; no gain (Parallel with Graal: 301 vs 234 us). |
| Platform thread pool (16) | 378 vs 293 us CPU/request, p99 2.7 s. |
| Compact object headers | No measurable change (228 vs 226 us). |
| Larger young generation (`-Xmn600m`) | No CPU gain, +230 MiB RSS, longer worst-case pauses. |
| `-XX:+AlwaysPreTouch` | No CPU gain (215 vs 216 us); RSS starts at ~1.26 GB instead of growing to ~1.2 GB. |
| Bigger heap (1.4 GB, 1.75 GB) | No CPU gain (217-221 vs 219 us); costs RSS and takes ~125 MiB of page cache from SQLite. |
| Fewer reader connections (1 or 2 instead of 4) | No change (219-222 us): with `mmap`, re-reading pages after another connection's write is cheap. |
| Group commit | Not needed: with WAL + `synchronous=NORMAL` a commit doesn't fsync, and writes are ~8% of requests. |
| WebFlux / JPA / Spring Security / Jackson | Blocking SQLite would need a thread hop per request; JPA and Security add per-request work for nothing this API needs. |

## Results (local)

Docker on Windows (WSL2), server container `--cpus=1 --memory=2g` running exactly this `start.sh`, k6 in separate
containers. One desktop core is roughly twice a DigitalOcean vCPU, so these overstate the real score; the relative
comparisons are what carry over.

| Load | CPU/request | p95 | p99 | Errors | Peak RSS |
|---|---|---|---|---|---|
| **This build, 30k users, 5-minute hold (~2,900 req/s)** | **184 us** | **3.1 ms** | **19 ms** | **0%** | 821 MiB |
| Earlier build (before statement cache, filters, THP), same hold | 218 us | 1.7 ms | 13.9 ms | 0% | 824 MiB |
| Earlier build, same JVM kept running, 3 more 5-minute windows | 212-214 us | 1.5-1.6 ms | 12.6-13.1 ms | 0% | plateau ~1.2 GiB |
| 15,000 idle keep-alive connections | - | - | - | 0 failed | +104 MiB |

At 30k users this build uses about 53% of one (desktop) core. No degradation over 25 minutes of continuous load;
the WAL file stays at ~4 MB.

The two holds ran in different sessions, and latency on a shared desktop host moves with whatever else the machine
is doing, so compare their CPU/request rather than their percentiles. The like-for-like comparisons (same session,
fresh JVM each, run side by side or in swapped order) are the ones quoted under "Optimizations".

## How this was measured

- `test/test.sh` (42 checks) in an `ubuntu:24.04` container with `--cpus=1 --memory=2g`, from `install.sh` on a
  clean image: build ~40 s, peak container memory ~1.1-1.5 GB.
- Every comparison ran on a freshly started JVM and a fresh copy of the seed database, warmed up for 2 minutes at
  1,000 users (as in scoring), then loaded with `bench/load.js`, with the running JVM's command line logged to
  rule out a stale process. Comparisons were repeated in swapped order or run side by side so host drift cancels.
- Profiles: JFR in the box under load, plus IntelliJ's profiler; both put SQLite `prepare` at ~40% of SQLite time.

## License

MIT, under the repo's [license](../../LICENSE).
