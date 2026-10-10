# Java 25 and Spring Boot 4 submission

A simple Spring MVC implementation behind Nginx. SQLite is mandatory under
challenge rule 2. PostgreSQL and external cache services are not allowed.

## Versions

| Component | Version |
| --- | --- |
| Java | Eclipse Temurin 25.0.1+8 |
| Spring Boot | 4.0.3 |
| SQLite JDBC | Xerial 3.53.4.0 |
| Maven | 3.9.11 |

Spring Boot's dependency management pins Tomcat, Jackson 3, HikariCP, Spring JDBC,
and test dependencies. The POM pins the formatter and its Java formatter engine.
`install.sh` verifies the pinned Java and Maven archives against embedded checksums.

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
The app opens that SQLite file in-process. Nginx publishes port 3000 on localhost
and forwards requests to the app container. Only Nginx has a published port.
The app runs as UID 10001 with one CPU and a 1 GiB container memory limit.
The Compose JWT secret is the public challenge secret. This is not a production
authentication setup. Do not expose it publicly with that secret.

The image build runs formatting checks, compilation, HTTP integration tests, and
JAR packaging. The optional `test` service runs the unchanged repository
`test/test.sh` against Nginx. Run it once per fresh database, since the golden
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

Read logs with `docker compose logs app nginx seed`. The local Nginx configuration
copies the challenge's upstream keep-alive and proxy settings, except that its
upstream uses the Docker service name rather than `127.0.0.1`.
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
docker compose --profile test run --rm --no-deps test -c 'curl --noproxy "*" http://nginx/health'
```

If this returns healthy JSON, inspect your host's Docker or WSL port-forwarding
configuration rather than changing the API. Do not delete the data volume to
diagnose a network problem.

## Automated local limit search

The wrapper needs Python 3.10+ on Linux or WSL, Docker Compose with JSON
configuration output, and a Docker host using cgroup v2 for resource counters.
The app, seed generator, and k6 run in containers. It does not install Java or k6
on your host. From this submission directory, run:

```bash
python3 benchmark.py
```

By default, it builds the source with the existing Dockerfile, including its
formatting and integration tests, using project-specific image tags. It does not
replace the regular stack's image tags. Build images and cache are retained.
The Maven proxy settings described above still apply. Use `--reuse-images` to
reuse the local `twelve-dollar-java-app:latest` and `twelve-dollar-java-seed:latest`
images instead. Build them first with `docker compose build app seed`; reused
images may not match the current source. Their image IDs are resolved before
startup, so later re-tagging cannot change this search. k6 2.2.0 is pinned by digest and
is pulled only if the image is missing.

Every search owns a unique Compose project and a fresh seeded database volume.
The database is fresh per search, not per run. Warm-up and later runs share the
instance, so benchmark posts and likes accumulate.
It does not stop, reset, or test against your regular stack. Port 3001 is used for
localhost health checks; choose another with `--port` if it is occupied. k6 sends
the unchanged `bench/load.js` workload over the Compose bridge through Nginx.
The app has the existing one-CPU/1-GiB limit, with app swap disabled. This does not
emulate the challenge's complete one-vCPU/2-GiB VM. Nginx, k6, and other host
processes still share your local machine.

The default search performs:

1. A 1,000-user warm-up with a 2-minute hold, excluded from the score.
2. 1-minute probes starting at 2,500 users, doubling until failure or 20,000 users.
3. Refinement of the passing/failing interval to within 250 users.
4. A 5-minute confirmation hold. A failed confirmation steps down 250 users.

Each run also has a 60-second ramp-up, the workload's fixed 30-second ramp-down,
and graceful completion. Short probes reduce search time but do not confirm a
score. Use `--probe-seconds 300 --budget-seconds 10800` for 5-minute probes with a
larger budget. The default load-search budget is
one hour, excluding image builds and startup. The wrapper does not begin a run
unless the remaining budget can fit its stages, a conservative 30-second graceful
completion allowance, and a 30-second startup allowance. A run that exceeds the
remaining budget is stopped and reported as budget exhaustion.

For example, increase the cap and require three consecutive confirmations:

```bash
python3 benchmark.py --reuse-images --max-vus 40000 --confirm-runs 3 --budget-seconds 7200
```

`--start-vus` and `--max-vus` must be multiples of `--step`. A successful result
at the user cap is reported as a lower bound, not a discovered maximum. A budget
stop leaves the search incomplete. There is no automatic resume, and each new
invocation starts a fresh instance.

### Reports and retained evidence

Reports update after each run in a new, ignored `benchmark-results/<run-id>/`
directory. Use `--output <new-directory>` to choose another location. Existing
output directories are never overwritten.

- `report.md`: run overview, threshold and endpoint-check outcomes, charts,
  confirmation spread, resource measurements, and environment settings.
- `report.html`: the same results with embedded SVG charts; no JavaScript,
  external fonts, or CDN dependencies. Log links need the accompanying files.
- `results.json`: normalized results and search status.
- `k6/run-*.json`, `run-*.log`, and `k6/run-*.events.jsonl`: raw k6 summaries,
  console output, and structured events. Only the `k6/` subdirectory is writable
  from the load generator; Compose configuration and reports remain outside it.
  Build, startup, stack, and cleanup logs are also
  retained when those steps run.
- `p95.svg`, `p99.svg`, `throughput.svg`, and `errors.svg`: chart files linked from
  Markdown and embedded in HTML.

The challenge thresholds are p95 below 500 ms, p99 below 1,000 ms, and fewer than
1% failed requests. JSON summaries and exit codes determine the benchmark result;
console text is not used to extract metrics. The wrapper also verifies that k6
reached the requested user count and ran for at least the requested stage time.
Endpoint checks have separate
passed/failed counts. An unexercised check is not reported as successful, and a
check failure does not independently change the challenge's threshold verdict.
The wrapper does not replace the 42-check correctness suite.

Execution errors, missing summaries, zero work, structured k6 error events, and
app restarts/OOM kills stop the search rather than being treated as capacity
failures. Reports distinguish those outcomes from threshold failures. Missing
optional memory/swap peak counters are reported as unavailable, not as zero.
Rootless Docker, user-namespace remapping, and SELinux bind-mount restrictions
have not been validated; the container user must be able to write the `k6/` mount.
Ctrl-C, SIGTERM, or SIGHUP retains completed results and attempts cleanup.
Cleanup stops any still-running k6 container before deleting only the
search's containers, network, and database volume, never its report directory or
the regular stack. SIGKILL or loss of Docker can prevent cleanup; use the saved
`compose.json` and project name to remove that search afterward.

The project name is `environment.project` in `results.json`. For manual cleanup:

```bash
docker compose -p <project> -f benchmark-results/<run-id>/compose.json --profile bench down --volumes --remove-orphans
```

Exit codes are 0 for a confirmed result, including a capped lower bound; 1 when
no candidate passes confirmation; 2 for budget exhaustion, execution errors, or
cleanup failures, invalid arguments, or an unavailable output directory; and 130
for interruption. A cleanup failure after interruption returns 2.

### Test the wrapper without a full search

```bash
python3 -m unittest -v test_benchmark
python3 benchmark.py --reuse-images --warmup-vus 50 --start-vus 50 --max-vus 100 --step 50 \
  --warmup-seconds 8 --probe-seconds 8 --confirm-seconds 10 --ramp-seconds 1 --budget-seconds 400
```

The second command exercises containers, summary parsing, reports, and cleanup.
Its short confirmation is explicitly marked as a smoke test. It is not a
performance result comparable to a 5-minute hold. See `python3 benchmark.py --help`
for all controls.
Changing `--ramp-seconds` from 60 also marks the result as non-comparable to the
default workload, even with a full-length confirmation.

## Benchmark VM contract

The challenge runs the required scripts on Ubuntu 24.04 x86_64 without Docker:

1. Run `sudo bash install.sh` once to install the pinned toolchain under `/opt`.
2. Run `bash build.sh` as a normal user to build from source and execute tests.
3. Set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT` from `SPEC.md`.
4. Run `bash start.sh`. It replaces the shell with the foreground Java process.

Use `HOST=127.0.0.1` and `PORT=3000` behind the provided benchmark Nginx.
`SQLITE_PATH` must name an existing copy of the seed, not the original seed file.
The app does not create or migrate the production schema. `JWT_SECRET` is required.

## First-draft decisions

- Spring MVC handles routing and Jackson writes compact JSON. Explicit property
  order preserves the exact challenge response bytes.
- `JdbcTemplate` executes the reference SQL with bound parameters. The like route
  first checks whether the post exists, so it runs two statements. There is no ORM,
  application cache, table copy, or schema change.
- The pool reuses up to four SQLite connections and keeps at least one idle.
  Each connection enables foreign keys, WAL, `synchronous=NORMAL`, and a five-second
  busy timeout. SQLite still serializes writers.
- Each insert uses auto-commit. JDBC closes the statement and its `RETURNING`
  result before the handler returns, so a success response follows its commit.
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
- Tomcat keeps idle upstream connections for 70 seconds, above the required
  65 seconds. The Java heap is capped at 512 MiB, leaving room for native memory,
  SQLite, threads, Nginx, and the OS. This is not a total process memory cap.

See [OPTIMIZATIONS.md](OPTIMIZATIONS.md) for possible later changes. None of those
ideas is a measured performance claim or part of this first draft.

## Verification

The first draft passes all 42 checks in the unchanged challenge suite through
Compose's Nginx. The source-built container also passes 25 HTTP integration tests,
including tied feed timestamps, Unicode length limits, concurrent duplicate likes,
independent-connection commit checks, connection pragmas, and degraded health.
Formatting checks and ShellCheck pass. No load-test score has been measured.
