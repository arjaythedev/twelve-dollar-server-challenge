# Java / Flash

Feed API implemented with Java 21, [Flash](https://github.com/Relism/Flash)
3.0.4 and SQLite through sqlite-jdbc 3.53.4.0.

**Direct HTTP, without Nginx.** Uses Flash's transport, annotated handlers,
routing, dependency injection and application lifecycle.

## Build and run

On Ubuntu 24.04 x86_64:

```bash
sudo bash install.sh
bash build.sh
HOST=127.0.0.1 PORT=3000 SQLITE_PATH=/path/to/feed.db \
  JWT_SECRET=twelve-dollar-challenge bash start.sh
```

The database must already contain the challenge schema and seed. All four
environment variables are required. For direct deployment, set HOST=0.0.0.0
and PORT=80 with the service's permission to bind privileged ports.

`install.sh` installs checksum-verified Temurin 21.0.11+10. `build.sh` downloads
checksum-verified Maven 3.9.16, runs the tests and builds `target/server.jar`.
`start.sh` runs the server in the foreground.

## Implementation

- SQLite builds complete feed and post JSON as UTF-8 BLOBs. JDBC returns byte
  arrays directly to Flash, avoiding result DTOs and response serialization in Java.
- One platform thread owns the SQLite connection, prepared statements and HMAC
  state. Request virtual threads wait through a bounded queue of 4,096 operations.
- Opportunistic batches of up to 64 operations amortize write commits without
  waiting to fill a batch. Responses from write batches wait for successful COMMIT.
- WAL with synchronous=NORMAL, exclusive locking, a 500-page SQLite cache and
  up to 1 GiB mmap. The schema is unchanged.
- Every protected request verifies its JWT signature and validates its claims.
  No response, query-result or token-verification caches.
- Serial GC, a 128–1,280 MiB heap and one active processor. Maximum request body
  size is 16 KiB; idle keep-alive timeout is 75 seconds.

## Validation

All 42 official checks and eight additional tests pass. The additional tests cover
JSON/Unicode/JWT validation, concurrent likes, rollback and database-owner failures.

From the repository root:

```bash
bash test/run.sh submissions/java-flash-relism
```

The unchanged official k6 workload completed a 10,000-VU, five-minute hold after
warm-up. The server ran in Docker with one CPU, 2 GiB RAM and no swap; k6 ran
outside Docker on the same host. Percentiles include ramp-up and ramp-down.

| Metric | Result |
|---|---:|
| Requests | 372,860 |
| Failed requests / checks | 0 / 0 |
| p95 | 107.47 ms |
| p99 | 467.08 ms |
| Peak server cgroup memory | 928 MiB |

## License

MIT, as specified by the challenge repository.
