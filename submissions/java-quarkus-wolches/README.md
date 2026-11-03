# Java + Quarkus

| | |
|---|---|
| Language | Java 25 (Eclipse Temurin 25.0.3+9, installed by `install.sh`) |
| Framework | Quarkus 3.40.1: Quarkus REST (RESTEasy Reactive) on Vert.x, Jackson for JSON |
| SQLite driver | `quarkus-jdbc-sqlite` 3.0.11 (Quarkiverse) over `sqlite-jdbc` 3.46.1.3, which bundles SQLite 3.46.1; Agroal 2 for the data sources |
| JWT | `quarkus-smallrye-jwt` (SmallRye JWT 4.6.4 / jose4j), HS256 only |
| Build | Apache Maven 3.9.16; Lombok 1.18.48 at compile time only |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself. Works behind Nginx too: `HOST=127.0.0.1 PORT=3000` |

All dependency versions come from the Quarkus 3.40.1 BOM plus the two explicit versions in `pom.xml`.

## Running it

```bash
sudo bash install.sh   # Temurin JDK 25 and Maven 3.9.16 into /opt, checksums verified
bash build.sh          # mvn package: target/quarkus-app/quarkus-run.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` wraps `JWT_SECRET` into the `oct` JWK that SmallRye JWT expects and sets
`-Xms512m -Xmx1g -XX:+UseSerialGC` for the 1 vCPU / 2 GB box (override with `JAVA_OPTS`).

## How it is built

- **Non-blocking endpoints, two dedicated database threads.** Every endpoint returns a Mutiny `Uni`,
  so the Vert.x event loop never blocks on SQLite. All queries run on two single-threaded Vert.x
  worker executors: one owns the **writer** connection, one owns the **reader** connection. On one
  vCPU this is as much parallelism as SQLite can use (one writer at a time in WAL mode), and it means
  no connection pool contention and no lock waits inside the application.
- **Separate data sources for reads and writes.** The reader opens the file read-only
  (`open_mode=1`) and the writer has `foreign_keys=true`; both use `journal_mode=WAL` and
  `busy_timeout=5000`. Each pool is sized to exactly one connection, which the worker holds for the
  life of the process.
- **Prepared statements are cached per connection** and reused for every request; the SQL is the
  reference SQL from SPEC.md. Result sets are always closed, and every job ends with an explicit
  `commit` or `rollback` whatever its outcome, so a 404 can never leave a read transaction open and
  pin the reader to a stale snapshot.
- **Likes are one statement.** `INSERT ... ON CONFLICT DO NOTHING`; the update count tells
  "first like" (201) from "already liked" (200). A like on a missing post fails the foreign key, and
  that SQLite error code is mapped to 404 `post not found`, so there is no extra existence query.
- **Lazy authentication.** `quarkus.http.auth.proactive=false`: the JWT is parsed only for the two
  `@Authenticated` POST endpoints, never for `/feed`, `/posts/:id` or `/health`. A request filter
  then checks the payload (`sub` a positive integer string, `username` a string) only on those
  endpoints, which keeps the "auth before id" order the spec requires.
- **Error bodies through exception mappers.** Domain exceptions carry their status and message;
  Jackson parse errors are unwrapped from the `WebApplicationException` Quarkus raises so they map to
  `malformed JSON body`; unknown paths map to `not found`. Response records use
  `@JsonPropertyOrder`, so key order matches the golden files byte for byte.
- **JVM.** Serial GC and a bounded heap: on one core the concurrent collectors only add threads that
  compete with the event loop, and 2 GB is shared with the OS. Native access is enabled for the
  SQLite JNI library so no warning is printed at start.

## Tests

`src/test` is a Java port of `test/test.sh` (same checks, same byte-exact golden comparisons) and of
`seed/make-seed.sh`: `mvn test` builds `seed/feed.db` on the first run if it is missing, verifies the
same content checksum, starts the app on a fresh copy and runs the suite with RestAssured.

## License

MIT, under the repo's [license](../../LICENSE).
