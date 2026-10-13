# Java 25 and Spring Boot 4 submission

A simple Spring WebFlux implementation that serves clients directly, without Nginx
(challenge rule 9). SQLite is mandatory under challenge rule 2. PostgreSQL and
external cache services are not allowed.

## Versions

| Component | Version |
| --- | --- |
| Java | Eclipse Temurin 25.0.1+8 |
| Spring Boot | 4.0.3 |
| SQLite JDBC | Xerial 3.53.4.0 |
| Maven | 3.9.11 |

Spring Boot's dependency management pins Reactor Netty, Jackson 3, and test
dependencies. The POM pins the formatter and its Java formatter engine.
`install.sh` verifies the pinned Java and Maven archives against embedded checksums.

## Make workflow

Run `make help` from this directory. Builds use your installed Maven and Java 25.
The challenge suite and benchmark run in Docker by default (`MODE=docker`), on
this host (`MODE=direct`), or in Kubernetes (`MODE=k8s`). Reports use local
Python 3.10+. The same commands run
natively on ARM64 Macs and AMD64 Linux.

```bash
make all               # build, challenge suite, warm-up, limit search, reports
make all MODE=direct
```

| Target | Action |
| --- | --- |
| `make build` | Run `mvn clean verify`: formatting check, Java tests, `target/server.jar` |
| `make test` | Run `make build`, then the challenge suite |
| `make test-suite` | Run the unchanged 42-check `test/test.sh` against a fresh database |
| `make benchmark` | Warm up the app, search its load limit, and write reports |
| `make benchmark-smoke` | Run a short benchmark that checks the tooling; not a score |
| `make report` | Regenerate the latest report, or the one in `REPORT_DIR` |
| `make image` | Build native app, seed, and test images |
| `make up`, `make down`, `make logs` | Manage the regular Docker stack on `LOCAL_PORT` (default 3000) |
| `make run` | Run the JAR with installed Java; requires `SQLITE_PATH` and `JWT_SECRET` |

### Modes

`MODE=docker` starts a disposable Compose project. A seed container creates a
fresh database. The app runs in one `server` container limited to 1 vCPU, 2 GiB
of memory without swap, and 65,535 open files, the budget of the challenge VPS.
It serves port 80 directly, as on the VPS. The challenge suite and k6 run in separate containers on the same network,
outside that limit. Docker uses images for its own architecture. Each run deletes
only its own containers, network, and volume. Set `BUILD_IMAGES=false` to reuse
images from `make image`; `make all` does this.

`BENCH_K6=local` keeps the server in Docker but runs k6 on this host against the
published port. Use it when k6 needs more memory than the Docker VM has left; at
20,000 users k6 needs several GiB. Installed k6 v2 is used when present;
otherwise k6 2.2.0 is downloaded to a temporary directory. Requests then cross
Docker's port forwarding, which adds some latency on macOS.

`MODE=direct` runs the built JAR on this host against a fresh copy of
`seed/feed.db`. If the seed is missing, Node and sqlite3 generate it. The
challenge suite needs curl, jq, and openssl. The benchmark runs k6 on the host
against the app. Installed Java 25 and k6 v2 are used when present. Otherwise the
script downloads Java 25.0.1 and k6 2.2.0 into a temporary directory. Every download is
checked against a pinned SHA-256. The script deletes that directory and stops
its processes afterwards. Direct mode does not limit the app's CPU or memory.

`MODE=k8s` runs in namespace `K8S_NAMESPACE` (default `twelve-dollar-challenge`)
of the current kubectl context. The cluster needs pod-level resources (Kubernetes
1.34 or later). The
script builds the app and seed images for the nodes' architectures, pushes them to
`K8S_REGISTRY`, and applies `k8s/` with the images pinned by digest. A `server` pod
runs the app, with the seed step as an init container. A pod-level limit makes
both share 1 vCPU and 2 GiB. k6 runs in
its own pod on a different node and reaches the pod through the `server` Service.
That pod's init container takes `load.js` and the deterministic seed tokens from
the seed image, so no files are sent through `kubectl exec`.
A `kubectl port-forward` carries only health checks and the challenge suite, which
runs on this host and needs curl, jq, and openssl. Each run replaces the pods, so
it starts from a fresh database, and deletes them afterwards. The namespace must
already exist. For a private registry, set `K8S_PULL_SECRET` to an image pull
secret in that namespace; the script checks for it. It never deletes the
namespace or the secret. Parallel runs need one namespace each. Scheduling rules
give every app pod a node without another benchmark pod from any namespace, and
keep load generators off app nodes; a run stops after 10 minutes when no such node
is free. Load generators of parallel runs may share a node. Run `docker login` for the registry first:

```bash
make benchmark MODE=k8s K8S_REGISTRY=<registry host>/<owner> K8S_PULL_SECRET=<secret>
```

To avoid repeating these settings, copy `.env-sample` to the git-ignored `.env`.
With [direnv](https://direnv.net), `.envrc` loads it into the environment, where
make picks it up.

Every mode picks free localhost ports, so it does not conflict with `make up`.

### Benchmark

The mode script deploys the app and passes its URL to `benchmark.py`. The tool
waits for `/health`, then runs:

1. A warm-up of the running app with 1,000 users for 2 minutes, excluded from the score.
2. 1-minute probes from 2,500 users, doubling until a failure or 60,000 users.
3. Refinement between the last pass and the first failure to within 250 users.
4. A 5-minute confirmation. A failed confirmation steps down 250 users.

Each run adds a 60-second ramp-up and the workload's 30-second ramp-down. In
Kubernetes mode, the k6 pod persists between runs, so each run first waits until
the previous run's closed connections release their local ports, which Linux
holds for 60 seconds. Otherwise two consecutive runs above about 32,000 users
exhaust k6's 64,511 ports and fail to connect. The search budget is one hour after
the app is healthy. The tool never starts a run that cannot finish within the
budget. A run passes with p95 below 500 ms, p99 below 1,000 ms, and fewer than 1%
failed requests. A pass at the user cap is a lower bound, not a maximum.

Override the defaults shown by `make help`:

```bash
make benchmark BENCH_MAX_VUS=40000 BENCH_CONFIRM_RUNS=3 BENCH_BUDGET_SECONDS=7200
make benchmark MODE=direct BENCH_OUTPUT=benchmark-results/my-run
make report REPORT_DIR=benchmark-results/my-run
```

`BENCH_ARGS` passes extra options to `benchmark.py`. See
`python3 benchmark.py --help`.

The tool samples the app every 2 seconds during each run. Docker mode reads the
`server` container's cgroup CPU, memory, and OOM kills, and records the enforced
limits in the report. Kubernetes mode reads the app container's cgroup and records
the pod-level limits. That sum leaves out the seed step's page cache, which still counts toward
the pod's 2 GiB. Direct mode reads the Java process's CPU time and resident
memory. An app exit, restart, or OOM kill stops the search as an execution error,
not a capacity failure. So do k6 errors, k6 being killed for lack of memory,
missing summaries, and runs that miss the requested users or duration.

Each search writes to a new `benchmark-results/<run-id>/` directory, which Git
ignores. The files update after every run:

- `report.md` and `report.html`: result, runs, thresholds, endpoint checks,
  resources, charts, and settings.
- `results.json`: the data that `make report` renders.
- `run-*.log`, `k6/run-*.json`, and `k6/run-*.events.jsonl`: raw k6 output.
- `p95.svg`, `p99.svg`, `throughput.svg`, and `errors.svg`: charts.
- `jfr/run-*.jfr`: with `PROFILE=true` in Docker or Kubernetes mode, a Java Flight
  Recorder profile of each run, started and stopped with `jcmd`. The recording
  adds a little CPU load. Inspect it with `jfr view hot-methods <file>` or JDK
  Mission Control.

The exit code is 0 for a confirmed result, including a capped lower bound, 1
when no candidate passes, 2 for an execution error or exhausted budget, and 130
when interrupted. Ctrl-C keeps finished results and still removes the deployment.

A local result is not a challenge score. k6 and the app share your
machine, and a Mac measures its own CPU or Docker VM. Compose does not run k6
inside the app's limits either.

### Run the app yourself

`make run` needs a working copy of the seed. Never point it at `seed/feed.db`:

```bash
SQLITE_PATH=/absolute/path/to/copy.db JWT_SECRET=twelve-dollar-challenge make run
```

## Run locally with Docker Compose

You need Docker Engine and Docker Compose. You do not need Java, Maven, Node,
or SQLite installed on your host. From this submission directory, run:

```bash
docker compose up --build -d --wait
curl http://127.0.0.1:3000/health
docker compose --profile test run --rm test
```

The seed container generates the repository's deterministic seed and verifies its
content checksum. It installs a fresh copy and the seed tokens in a named volume.
The app opens that SQLite file in-process. The `server` container runs the app
as UID 10001 on port 80, published on localhost port 3000. It is limited to 1 vCPU
and 2 GiB of memory without swap.
The Compose JWT secret is the public challenge secret. This is not a production
authentication setup. Do not expose it publicly with that secret.

The image build runs formatting checks, compilation, HTTP integration tests, and
JAR packaging. The optional `test` service runs the unchanged repository
`test/test.sh` against the app. Run it once per fresh database, since the golden
checks expect the original seed and the suite writes posts and likes.

The database survives container restarts. Stop the stack without deleting it:

```bash
docker compose down
```

To repeat the golden tests, stop the stack and delete its local data volume.
The following command deletes all posts and likes added to this Compose database:

```bash
docker compose down --volumes
docker compose up --build -d --wait
docker compose --profile test run --rm test
```

Read logs with `docker compose logs server seed`.
Compose is a development setup, not evidence of a benchmark score.

If your network requires an HTTP proxy, Maven does not read `HTTPS_PROXY` itself.
Configure Docker's client proxy settings for the build's `apt` and `curl` calls.
Set `MAVEN_PROXY_ENABLED=true`, `MAVEN_PROXY_HOST`, and `MAVEN_PROXY_PORT` before
the image build. Compose passes these values to Maven's build-stage settings,
not the running server. The default disables the proxy. This optional Maven
configuration supports an unauthenticated proxy.

If the published localhost port times out, check the proxy without host port
forwarding first:

```bash
docker compose --profile test run --rm --no-deps test -c 'curl --noproxy "*" http://server/health'
```

If this returns healthy JSON, inspect your host's Docker or WSL port-forwarding
configuration rather than changing the API. Do not delete the data volume to
diagnose a network problem.

## Benchmark VM contract

The challenge runs the required scripts on Ubuntu 24.04 x86_64 without Docker:

1. Run `sudo bash install.sh` once to install the pinned toolchain under `/opt`.
2. Run `bash build.sh` as a normal user to build from source and execute tests.
3. Set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT` from `SPEC.md`.
4. Run `bash start.sh`. It replaces the shell with the foreground Java process.

Use `HOST=0.0.0.0` and `PORT=80`: the app serves clients directly, without Nginx.
It needs `CAP_NET_BIND_SERVICE` for port 80 and `LimitNOFILE=65535`, which the
challenge's service provides.
`SQLITE_PATH` must name an existing copy of the seed, not the original seed file.
The app does not create or migrate the production schema. `JWT_SECRET` is required.

## Design decisions

- Spring WebFlux on Reactor Netty handles routing and Jackson writes compact JSON.
  Explicit property order preserves the exact challenge response bytes.
- The app serves clients directly on Netty instead of behind Nginx, which saves
  Nginx's CPU and a second HTTP hop. Each client holds its own keep-alive
  connection, and Netty closes connections idle for 70 seconds. Netty holds no
  buffers for an idle connection, so the VPS's 65,535 open files bound the number
  of clients rather than memory.
- SQLite calls block, so they never run on Netty's event loops. Each SQLite
  connection has its own thread, and handlers continue when a result is ready.
- Three read-only connections (`PRAGMA query_only`) serve reads. One writer
  connection serves every write, so writes queue in Java instead of colliding in
  SQLite, where a waiting writer sleeps and retries. Each connection prepares its
  statements once. Statements cache query plans, never results, which rule 5
  allows. The like route first checks whether the post exists, so it runs two
  statements. There is no ORM, application cache, table copy, or schema change.
- SQLite builds the feed and post responses with `json_object` and
  `json_group_array`, and the app sends those bytes unchanged. SQLite writes compact
  JSON in the given key order and escapes quotes, backslashes, and control
  characters. The feed's `ORDER BY` sits inside the aggregate, which fixes the
  array order. Jackson writes the other responses.
- Each connection enables foreign keys, WAL, `synchronous=NORMAL`, and a five-second
  busy timeout. sqlite-jdbc's generated-key lookup is off, because the app reads
  new ids from `RETURNING`.
- Each insert uses auto-commit. The repository closes the `RETURNING` result, which
  resets the statement and completes the commit, before the handler returns.
  The existing unique constraint and `ON CONFLICT DO NOTHING` make likes idempotent.
- Every authenticated request verifies its JWT with JDK HMAC-SHA256 and a
  constant-time signature comparison. It accepts only HS256 and validates
  expiration and claim types. Unsupported critical JWT header extensions are
  rejected. Authentication runs before body or post-ID validation.
- Post lengths count Unicode code points, matching SQLite's `length` constraint.
  Java's `strip()` removes leading and trailing whitespace.
- After authentication, the app reads at most 16 KiB of a post request plus one
  byte. It returns compact JSON with status 413 for larger bodies, which the
  challenge leaves out of scope.
- The Java heap is capped at 1.25 GiB. That leaves room for native memory, Netty's
  pooled direct buffers, SQLite, kernel socket memory, the page cache, and the OS.
  The heap grows only as needed. This is not a total process memory cap.
  `start.sh` selects the Serial collector explicitly: on one CPU it has the lowest
  overhead, and JDK 27 would otherwise default to G1.

See [OPTIMIZATIONS.md](OPTIMIZATIONS.md) for possible later changes. None of those
ideas is a measured performance claim.

## Verification

The app passes all 42 checks in the unchanged challenge suite in its Compose
container. The source-built container also passes 25 HTTP integration tests,
including tied feed timestamps, Unicode length limits, concurrent duplicate likes,
independent-connection commit checks, connection pragmas, and degraded health.
Formatting checks and ShellCheck pass.
