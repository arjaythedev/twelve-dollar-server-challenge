# Java 25, Spring Boot 4 and WebFlux submission

| | |
| --- | --- |
| Language | Java 25, Eclipse Temurin 25.0.1+8 from the official tarball, SHA-256 checked |
| Framework | Spring Boot 4.0.3 with Spring WebFlux 7.0.5 on Reactor Netty 1.3.3 (Netty 4.2.10) |
| SQLite driver | Xerial sqlite-jdbc 3.53.4.0, which bundles SQLite 3.53.4 |
| JSON | Jackson 3.0.4, only for likes, new posts, health, and errors |
| Build | Maven 3.9.11, SHA-512 checked. The POM pins every library, mostly through Spring Boot's dependency management |
| Nginx or direct | Direct. The app listens on `HOST:PORT`, `0.0.0.0:80` on the benchmark VM |

## Running on the benchmark VM

1. `sudo bash install.sh` installs Java and Maven under `/opt`.
2. `bash build.sh` builds the JAR as a normal user. It also checks formatting and
   runs the Java integration tests.
3. Set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT` as in `SPEC.md`.
4. `bash start.sh` replaces the shell with the Java process. The app answers
   `/health` within a few seconds.

Port 80 needs the `CAP_NET_BIND_SERVICE` capability that the challenge's service
grants. `SQLITE_PATH` must name a copy of the seed; the app never creates or
migrates the schema.

## Optimizations and why

The app is limited by CPU on one vCPU, so each change removes work per request.
Java Flight Recorder profiles of the load test guided the order.

- **Direct serving.** Clients connect to Netty, without Nginx in front. That
  removes Nginx's CPU and a second HTTP parse per request. Each client keeps its
  own keep-alive connection, and Netty closes connections idle for 70 seconds.
- **WebFlux on Netty.** Netty holds no buffers for an idle connection, so tens of
  thousands of idle clients cost little heap. Tomcat keeps about 12 KiB per open
  connection.
- **JSON built in SQLite.** One query returns the whole feed or post response
  through `json_object` and `json_group_array`, and the app sends those bytes
  unchanged. Feed and post reads are about 92% of the load, and Jackson was 12% of
  Java CPU before this change. SQLite writes compact JSON in the given key order;
  the `ORDER BY` inside the aggregate fixes the order of the posts.
- **Prepared statements, reused.** Each SQLite connection prepares its statements
  once. They cache query plans, never results, which rule 5 allows.
- **One writer, three readers.** One connection serves every write, so writes
  queue in Java instead of colliding in SQLite, where a waiting writer sleeps and
  retries. Three `query_only` connections serve reads. Each connection has its own
  thread, so blocking SQLite calls never run on Netty's event loops.
- **No generated-key lookups.** sqlite-jdbc would otherwise prepare and run
  `SELECT last_insert_rowid()` after every insert. New post ids come from
  `RETURNING`.
- **JVM settings.** `start.sh` sets a 1.25 GiB heap limit and the Serial
  collector, which has the least overhead on one CPU. JDK 27 would default to G1.

### Rules this keeps

- Every request reads SQLite while it is served. There is no response cache,
  query-result cache, table copy, or remembered JWT.
- WAL with `synchronous=NORMAL`. Every insert commits before its response, and
  there is no group commit.
- Every authenticated request verifies its HS256 signature and `exp`, with a
  constant-time comparison. Authentication runs before body and post-id checks.
- The schema is unchanged. Likes stay idempotent through the existing primary key
  and `ON CONFLICT DO NOTHING`.

### Local load-test results

These come from a Kubernetes test setup, not the challenge VM. The app pod has a
1 vCPU and 2 GiB limit, and k6 runs `bench/load.js` from another node. The numbers
compare the two versions; they do not predict the droplet's score. Both are the
most recent run of each version, with the same benchmark tool.

| Version | Result |
| --- | --- |
| Spring MVC with all optimizations above | 31,750 users held for 5 minutes: p95 395 ms, p99 729 ms, no errors |
| WebFlux with all optimizations above | 38,000 users passed a 1-minute probe: p95 21 ms, p99 46 ms. The 5-minute hold at 38,000 failed (p99 2,972 ms), and the one at 37,750 stopped on a k6 error |

In the WebFlux profile, Spring WebFlux, Reactor, and Netty account for about 90%
of the Java CPU samples. [OPTIMIZATIONS.md](OPTIMIZATIONS.md) lists the next
candidates.

## Behavior details

- Post lengths count Unicode code points, like SQLite's `length` check. Java's
  `strip()` removes surrounding whitespace.
- The app reads at most 16 KiB of a post body. Larger bodies, which the challenge
  leaves out of scope, get status 413 with a JSON error.
- Unknown paths and failures outside the controllers still return the challenge's
  JSON error bodies.

## Verification

- The unchanged 42-check `test/run.sh` passes on a clean Ubuntu 24.04 after
  `install.sh`, as the challenge's CI runs it.
- `build.sh` runs 27 HTTP integration tests. They cover exact response bytes, tied
  feed timestamps, escaping and Unicode in SQLite-built JSON, concurrent duplicate
  likes, commits visible to another connection, connection pragmas, read-only
  readers, and degraded health.
- ShellCheck passes on the scripts.

## Development tooling

None of this runs on the benchmark VM. `make help` lists every target and setting.

| Target | Action |
| --- | --- |
| `make build` | `mvn clean verify`: formatting check, integration tests, `target/server.jar` |
| `make test-suite` | The unchanged 42-check `test/test.sh` against a fresh database |
| `make benchmark` | Warm-up, load-limit search, and reports |
| `make benchmark-smoke` | A short run that checks the tooling, not a score |
| `make up`, `make down`, `make logs` | A Compose stack on localhost port 3000 |
| `make run` | Run the JAR with installed Java; needs `SQLITE_PATH` and `JWT_SECRET` |

`MODE` picks where the suite and benchmark run:

- `MODE=docker` (default) runs the app in one container limited to 1 vCPU, 2 GiB
  without swap, and 65,535 open files. k6 runs in its own container, or on the host
  with `BENCH_K6=local` when the Docker VM lacks memory for k6.
- `MODE=direct` runs the JAR and k6 on this host without limits. It downloads
  missing tools to a temporary directory and checks their SHA-256.
- `MODE=k8s` pushes images to `K8S_REGISTRY` and runs the app pod and a k6 pod in
  `K8S_NAMESPACE`. The pod-level limit is 1 vCPU and 2 GiB. Scheduling rules give
  each app pod a node without other benchmark pods, so parallel runs in separate
  namespaces do not share a machine. Copy `.env-sample` to `.env` for these
  settings; `.envrc` loads it with direnv.

The benchmark follows the challenge's scoring: a 2-minute warm-up with 1,000 users,
1-minute probes that double from `BENCH_START_VUS` until one fails, refinement to
within 250 users, and a 5-minute confirmation. A run passes with p95 below 500 ms,
p99 below 1,000 ms, and fewer than 1% failed requests. In Kubernetes mode, each run
first waits until the k6 pod's closed connections release their ports, because
Linux holds them for 60 seconds and k6 has about 64,500. `PROFILE=true` saves a
Java Flight Recorder profile of every run.

Results go to `benchmark-results/<run-id>/`, which Git ignores: `report.md`,
`report.html`, `results.json`, raw k6 output, charts, and profiles. A local result
is not a challenge score.
