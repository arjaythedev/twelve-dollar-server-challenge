# Ruby + Iodine + prepared SQLite

A direct HTTP server with Ruby application handlers, Iodine's native networking, and SQLite-generated JSON. One Ruby process serves the API and keeps idle connections open without Nginx.

| Component | Version |
|---|---|
| Ruby | 4.0.5 with YJIT |
| HTTP server | Iodine 0.7.59 |
| SQLite gem | sqlite3 2.9.6 |
| SQLite engine in the tested Linux gem | 3.53.2 |
| JSON parsing and small responses | json 3.0.2 |

The Ruby source archive is checksum-pinned in `install.sh`; all three bundle gems and their checksums are pinned in `Gemfile.lock`. HS256 verification uses Ruby's OpenSSL library.

## Run

From this submission directory:

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

`install.sh` builds Ruby with YJIT under `/opt/ruby`. `build.sh` installs the locked bundle into `vendor/bundle` and can run as a normal user. `start.sh` enables YJIT and starts Iodine directly on `$HOST:$PORT`; set the address and port for the deployment. Startup requires the existing seeded database and leaves its schema and indexes unchanged.

## Implementation

- [Iodine](https://github.com/boazsegev/iodine) parses HTTP and manages connections in native code. A small Ruby object implements the Rack response interface directly, without a Rails application or a Rack gem dependency. Routing, authentication, validation, SQL selection, and error handling remain Ruby code.
- One Iodine worker and one application execution thread share a SQLite connection and prepared statements. A mutex protects the complete handler, including a like's insert and existence check. No SQLite connection is inherited by a forked worker.
- Every data response queries the live database. SQLite builds the complete feed, post, and create JSON documents, avoiding intermediate Ruby hashes and another serialization pass. The feed selects the newest 20 posts and aggregates with explicit `created_at DESC, id DESC` ordering. Like counts come from the live `likes` table; an empty feed remains `{"posts":[]}`.
- Parameters are bound to reused prepared statements. Write queries using `RETURNING` are stepped through `SQLITE_DONE` before sending success, then reset in `ensure`. Read statements are reset after their single result row.
- SQLite uses WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 500-page cache, and a 512 MiB mmap limit. Prepared statements and database/OS page caches are used; response, query-result, table, and token-verification caches are not.
- Every authenticated request verifies an HS256 signature with a constant-time comparison, requires a numeric unexpired `exp`, and validates optional `nbf`. Authentication precedes body and post-ID validation. Body parsing checks UTF-8, JavaScript's whitespace set, and the 500-code-point limit.

Two small settings accommodate Iodine 0.7.59's HTTP serialization and options: the JSON header value begins with optional whitespace because Iodine writes the colon without a space; `timeout: 75, ping: 75` sets HTTP keepalive to 75 seconds because this release's timeout option checks the `ping` argument's type. `ping` itself applies to WebSocket/raw connections.

## Size

Physical source lines include blanks and comments; counts exclude lockfiles, tests, documentation, and installed library source.

| Measure | Rails PR #12 | Python PR #13 | This submission |
|---|---:|---:|---:|
| Application + server configuration | 164 (158 + 4 + 2) | 156 (144 + 12) | 150 (140 + 10) |
| Above + install/build/start scripts + dependency declaration | 197 | 176 | 182 |
| Bundle-resolved gems / installed Python packages | 68 | 3 | 3 |

Dependency counts describe the installed application bundles, excluding the language runtime and its standard libraries. The Ruby install script includes the source build and checksum verification.

## Local comparison

The comparison uses the current Rails implementation from [PR #12](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/12) and Python/Granian from [PR #13](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/13). The following results are medians of three runs per workload and implementation, with execution order rotated between trials: 27 measured runs in total.

Compared commits: Rails `9fafb3225f29a83b72ec5f27fb491762aad95eae`; Granian `164a5df9b68fdceb3c6637c75444cffc11f58737`.

Each trial starts from a fresh seeded database copied onto the container filesystem. `wrk -t2 -c64 --latency` runs a two-second warmup followed by 15 seconds of measurement. Mixed traffic uses relative weights of 100 feeds, 100 single-post reads, 15 likes, and two creates, with live feed IDs, seed JWTs, real writes, and no think time.

The environment is Ubuntu 24.04 ARM64 under Docker Desktop on an Apple M2 Pro. Each tested server stack is limited to one CPU and 2 GiB RAM without container swap; the load generator runs separately on other CPU cores. Iodine and Granian receive HTTP directly. Rails uses its documented Nginx deployment, with Nginx and Rails sharing the same CPU and memory limits. Ruby uses SQLite 3.53.2 from its pinned gem; Python uses standard-library SQLite 3.45.1. These are comparisons of the complete submitted stacks, so the numbers do not isolate language or HTTP-server overhead alone.

| Requests/second, higher is better | Rails PR #12 | Granian PR #13 | This submission |
|---|---:|---:|---:|
| Feed | 10,622 | 20,989 | 26,454 |
| Single post | 15,930 | 50,853 | 76,574 |
| Mixed reads/writes | 11,581 | 30,380 | 35,056 |

| p99 milliseconds, lower is better | Rails PR #12 | Granian PR #13 | This submission |
|---|---:|---:|---:|
| Feed | 12.79 | 5.59 | 3.05 |
| Single post | 9.02 | 2.41 | 1.15 |
| Mixed reads/writes | 15.41 | 11.73 | 12.18 |

All 27 measured trials reported zero HTTP/socket errors. Relative to Rails PR #12, median throughput
is 2.49× for feeds, 4.81× for single posts, and 3.03× for mixed traffic. Relative to Granian PR #13,
it increases by 26%, 51%, and 15%, respectively. Feed/post p99 is lower than both baselines;
mixed p99 is lower than Rails but slightly higher than Granian (12.18 vs 11.73 ms).

Median summed process RSS during mixed traffic was 183.82 MiB for Rails plus Nginx,
93.95 MiB for Granian, and 81.00 MiB for Iodine. Summing multiple processes can count shared pages
more than once; Iodine runs in one process.

These local throughput measurements do not establish an official x86_64 five-minute k6 user-capacity score.

## Validation

- All 42 official API checks passed.
- `check.py` passed 79 additional HTTP checks plus 16 concurrent duplicate-like requests, covering JWT claims and expiry, malformed JSON/UTF-8, Unicode, authentication ordering, concurrent creates, and live reads/counts.
- A forced `SIGKILL` followed by restart preserved acknowledged writes.
- A fresh dependency build as an unprivileged user succeeded.
- A separate direct-connection test opened and health-validated 15,000 sockets in 1.40 seconds, with about 146 MiB of server-process RSS. It reused 200 original sockets after 66.08–67.46 seconds of idle time without reconnecting.

Run the official suite from the repository root with a fresh seed database:

```bash
bash test/run.sh submissions/ruby-iodine-tbsvttr
```

Run the additional checks against a separately started server using another disposable seeded database. Both suites create posts and likes:

```bash
JWT_SECRET=twelve-dollar-challenge python3 submissions/ruby-iodine-tbsvttr/check.py http://127.0.0.1:3000
```

## Credits

[Gambitboy's Rails submission, PR #2](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/2), established the Ruby/Rails baseline. The prepared statements, SQLite JSON responses, validation, and write-completion handling build on [the compact Rails implementation, PR #12](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/12). The direct deployment and ordered feed aggregation also draw on [Python/Granian, PR #13](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/13).

MIT, under the repository's [license](../../LICENSE).
