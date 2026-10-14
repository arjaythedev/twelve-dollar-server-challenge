# Java + Quarkus + Panama (FFM)

| | |
|---|---|
| Language | Java 27 (Eclipse Temurin 27+35) |
| Framework | Quarkus 3.40.1 (Quarkus REST + Jackson, Vert.x 4.5.34, Netty 4.1.138, native epoll) |
| SQLite driver | none: Ubuntu's SQLite 3.45.1 called through Java's Foreign Function & Memory API |
| JSON | Jackson 2.21.7 + Blackbird |
| JWT | auth0 java-jwt 4.6.1 |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

`install.sh` checksum-verifies the JDK and Maven; the Quarkus BOM and `pom.xml` pin everything else.

## Running it

```bash
sudo bash install.sh   # Temurin 27 and Maven under /opt; apt: libsqlite3-0
bash build.sh          # target/quarkus-app/quarkus-run.jar
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **[Panama](https://youtu.be/2wHU5ocfFsA?list=RD2wHU5ocfFsA)** Panama faster then FFI for SQLlite
- **One event loop.** Endpoints are `@NonBlocking`, so reads run inline on a LIFO pool of read connections.
- **`synchronous=FULL` with group commit.** A 201 survives a power loss. Writes go to one writer thread, which waits
  2 ms after the first queued write, commits everything queued in one transaction (one fsync), then completes each
  request. Committing each write on the event loop failed at 64k users (fsync ~1.4 ms, ~530 writes/s).
- **Quarkus settings:** idle timeout 90 s, backlog 4096, HTTP/2 off, 16 KB output buffer (keeps `Content-Length`),
  16 KB request body limit.
- **Pragmas:** WAL, `synchronous=FULL`, 16 MiB page cache, `mmap_size=0`, `temp_store=MEMORY`.
- **JSON:** Jackson with Blackbird (~2% fewer instructions per request).
- **JVM:** G1, `-Xmx768m`, `MaxDirectMemorySize=128m`.

## Results on a $12 droplet

1 vCPU / 2 GB, load from a second droplet, the scoring procedure:

_(final numbers to be added)_

## License

MIT, under the repo's [license](../../LICENSE).
