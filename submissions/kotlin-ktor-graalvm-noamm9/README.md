# Kotlin + Ktor GraalVM

|                     |                                                                                                                                                           |
|---------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| Language            | Kotlin 2.4.0                                                                                                                                              |
| Runtime             | GraalVM CE 25.4.4.1.1 native image                                                                                                                        |
| Framework           | Ktor 3.6.0                                                                                                                                                |
| Server              | Ktor CIO                                                                                                                                                  |
| SQLite driver       | xerial sqlite-jdbc 3.53.4.0                                                                                                                               |
| Connection pool     | Own coroutine pool (`Channel`) of 10 connections with reused prepared statements                                                                          |
| JSON                | kotlinx.serialization through Ktor content negotiation                                                                                                    |
| **Nginx or direct** | **Direct preferred**: serves `0.0.0.0:80` when configured that way;<br>But can be tested over Nginx too by changing `PORT` environment variable to `3000` |

Dependency versions are pinned in `gradle/libs.versions.toml`

## Running it

```bash
sudo bash install.sh 
bash build.sh
SQLITE_PATH=... JWT_SECRET=.. HOST=0.0.0.0 PORT=80 bash start.sh
```

For local development, use a non-privileged port:
```bash
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

The server reads all runtime configuration from the challenge environment variables:

| Variable      | Purpose              | Default                   |
|---------------|----------------------|---------------------------|
| `SQLITE_PATH` | SQLite database path | `<project_dir>/sqlite.db` |
| `JWT_SECRET`  | HS256 JWT secret     | `twelve-dollar-challenge` |
| `HOST`        | HTTP bind host       | `127.0.0.1`               |
| `PORT`        | HTTP bind port       | `80`                      |

Defaults are provided only for local development.

## Build output

`install.sh` installs GraalVM CE into `/opt/graalvm`. `build.sh` compiles with Gradle (`installDist`), and once Gradle has exited runs `native-image` on the resulting jars. `start.sh` then runs the native binary in the foreground:

```bash
build/native/nativeCompile/velox
```

Reflection used by kotlinx.serialization, Ktor and slf4j is registered in `src/main/resources/META-INF/native-image/dev.noamm9.velox/velox/reachability-metadata.json`, generated with the GraalVM tracing agent while running `test/test.sh`. A new `@Serializable` response class needs its `Companion` field and `Companion.serializer()` added there.

## Optimizations, and why

- **Direct mode preferred.** The app can bind to `0.0.0.0:80` when the benchmark environment provides the required capability. This avoids spending CPU on Nginx on a one-vCPU machine. For development, `PORT=3000` can be used to avoid privileged-port binding issues.
- **GraalVM native image.** No JIT warm-up and a much smaller memory footprint than the JVM, which leaves more of the 2 GB for SQLite's page cache and connections.
- **Native build that fits the 2 GB box.** `kotlin-reflect` is excluded from the runtime classpath (Ktor pulls it in, nothing here needs it), which halves the code native-image analyses. Gradle compiles the Kotlin in-process and exits before `native-image` starts, and the builder's heap, code cache and metaspace are capped. On one CPU with 1700 MB and no swap the build peaks at 1.42 GiB RSS and takes about 8 minutes including Gradle.
- **Ktor CIO.** CIO is a coroutine-based HTTP engine and keeps the stack simple without an external servlet container.
- **Own SQLite connection pool with reused prepared statements.** 10 connections are opened at startup and handed out through a coroutine `Channel`, so waiting for a connection suspends instead of blocking a thread. Each connection prepares every query once and reuses it, which skips SQL parsing and planning on every request (allowed: it caches the plan, not the data).
- **SQLite pragmas set on connections.** Each pragma runs as its own statement (sqlite-jdbc ignores everything after the first statement in one string). The app enables WAL mode, `synchronous=NORMAL`, a busy timeout, mmap, a larger page cache, and in-memory temporary storage. WAL plus `synchronous=NORMAL` keeps writes durable enough for the challenge rules while improving read/write behavior.
- **Reference-style SQL.** Feed and post reads use the challenge's join plus per-post like-count query shape. Post creation uses `INSERT ... RETURNING`. Likes use one insert statement with `ON CONFLICT DO NOTHING`, followed by a post-existence check only when needed to distinguish an existing like from a missing post.
- **No cross-request response cache.** Requests read from SQLite while being served. Prepared statements and SQLite's own page cache are the only caching-like behavior used.
- **Hand-rolled HS256 JWT verification.** Signature (constant-time compare), `alg`, `exp`, `nbf` and `iat` are checked with `javax.crypto` and kotlinx.serialization instead of java-jwt, which removes Jackson and the Ktor auth plugins. Every token is verified on every request; nothing is cached.
- **INFO-level logging with slf4j-simple.** Nothing is logged per request, and unlike logback it needs no XML parser in the image.
- **Compact JSON via kotlinx.serialization.** Response objects are serialized by Ktor content negotiation with stable field order from the Kotlin serializable models.

## Credits

Based on [syorito-hatsuki's Kotlin + Ktor JVM submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/36) (routes, SQL and README structure), ported to a GraalVM native image with its own connection pool and JWT verification.

## License

MIT, under the repo's [license](../../LICENSE).