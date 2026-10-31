# Ruby + Rails Metal + prepared SQLite

Rails 8.1.4, Ruby 4.0.5 with YJIT, Puma 8.0.2, sqlite3 2.9.6 and jwt 3.3.0.
All gem versions and checksums are pinned in `Gemfile.lock`; the Ruby source archive is checksum-pinned.

**Direct HTTP:** Puma listens on `$HOST:$PORT`, with two request threads, its request reactor,
and a 75-second keep-alive timeout. Use `HOST=0.0.0.0 PORT=80` on the benchmark machine.
The application and server configuration total 194 physical lines, including the pressure helper.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Connection pressure — 2026-10-09

The Linux process descriptor count is sampled on the response path at most once
per 100 ms. At 16,000 open descriptors, responses advertise `Connection: close`;
the transport drains their headers and body before retiring the socket. Smaller
inherited limits reduce the budget with a reserve of 128. An `EMFILE`/`ENFILE`
sampling failure also triggers retirement. The sample includes non-socket
descriptors and incomplete uploads; it stores transport metadata, not API data.
On platforms without `/proc`, normal keep-alive remains available without this
Linux pressure guard. Successful writes commit before their replies.

Ordinary connection counts retain the 75-second keep-alive timeout. Clients
reconnect after a retired response. The resource budget is not a logical-user cap.


Puma now serves directly under rule 9, using its reactor for idle connections and
incomplete uploads. `max_keep_alive` is 1,000,000,000 instead of the default 999:
the default closes a long pipeline with unread requests still buffered. The
75-second idle timeout remains. Puma 8 bases its keep-alive decision on the Rack
request environment; the response path sets its connection-close flag after
database work. A response header alone does not retire its socket. The historical tables below measured the older
Nginx deployment; their recorded values are not measurements of this direct setup.

The source and throughput comparisons recorded before this update describe their
pinned baseline revisions. New evidence and exact source hashes are recorded in
[capacity-results.json](capacity-results.json).

The selected diagnostic served **30,000 logical users** for a **5-minute hold**, with **0 errors**, worst-shard p95/p99 **71.4/124.3 ms**, and sampled peak container memory **710.3 MiB**. The sum of process RSS high-water marks peaked at **345.0 MiB**; shared pages may be counted more than once. This is one local trial, not a maximum-capacity search.

Run correctness and recovery checks from the submission directory on a fresh seed:

```bash
python3 verify.py
```

With server hard/soft `nofile=256` and a higher client limit:

```bash
python3 tests/pressure_regression.py 3000
python3 tests/fd_exhaustion.py 3000
```

The first regression finishes an interrupted Unicode upload during retirement,
checks committed writes/readbacks, and confirms keep-alive returns when pressure
subsides. The second exhausts descriptors with incomplete HTTP headers before
completing one valid request and checking that its full response drains and closes.

For a sustained Linux diagnostic, against a running server and fresh seed copy:

```bash
mkdir -p bin
cc -O3 -Wall -Wextra tests/connection_load.c -lm -o bin/connection-load
bin/connection-load 3000 --users 70000 --seconds 300 --ramp 60 --tokens ../../seed/tokens.json
```

The C generator follows the feed/post/like/create loop, request probabilities and
think-time ranges in `bench/load.js`. It uses a different PRNG and post text,
several loopback source addresses, and no separate warm-up or ramp-down. It reports
hold-only request latency, latency including reconnects, and generator resource
usage. These are local diagnostics, not official k6/droplet scores or a search for
maximum capacity. The diagnostic writes posts and likes.

## Implementation

This is a Rails application with `Rails::Application`, Rails routing and an `ActionController::Metal`
controller. It includes Rails' controller callbacks and exception handling, while omitting rendering,
parameter wrapping and request instrumentation that this API does not use. The full Rails gem remains
pinned; only the Action Controller railtie is loaded. SQLite queries replace Active Record model objects.

- Reused prepared statements query the live database on every request. SQLite constructs feed, post and
  create JSON, avoiding intermediate Ruby model objects, hashes and serialization passes. The feed query
  first selects the newest 20 posts, then joins authors and aggregates JSON with an explicit timestamp/id
  order. Like counts still come from the live `likes` table.
- Two Puma request threads share one SQLite connection. A Rails `around_action` callback holds a mutex
  for the complete action, after authentication and ID validation. This protects prepared statements and
  keeps a like's insert and fallback existence check together, preventing a concurrent create from making
  a missing post look like an already-liked post. Socket handling can overlap between the Puma threads.
- WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 500-page cache and a 512 MiB mmap limit
  are configured inside the process. There are no worker processes sharing the connection.
- Each query returns at most one row. The helper binds parameters directly, steps through `SQLITE_DONE`
  and resets the statement in `ensure`. An `INSERT ... RETURNING` therefore commits before returning 201.
- JWT signatures and expiration are checked on every authenticated request. No token verification,
  response, row or query-result cache is used.
- The create action parses JSON after authentication. UTF-8, exact HS256, JavaScript whitespace trimming
  and the 500-code-point body limit are validated explicitly.
- Unused file-serving, timing, request-ID, client-IP, request-logging and dispatch-callback middleware is
  removed, along with ETag and conditional-response middleware. Rails' executor, exception middleware and
  HEAD handling remain. Metal omits Rails' default browser-security headers; JSON content type is explicit.
- Production disables class reloading and static-file serving. Logging is set to warning level.

## Validation

From the repository root, the official suite builds and starts the application with a fresh seed database:

```sh
bash test/run.sh submissions/ruby-rails-tbsvttr
```

`check.py` adds malformed JSON/JWT, Unicode, concurrent write and live-read checks against a running
server. Use a disposable seeded database; these checks create posts and likes:

```sh
JWT_SECRET=twelve-dollar-challenge python3 check.py http://127.0.0.1:3000
```

The optimized implementation passes all 42 official checks, 70 additional HTTP checks, 16 concurrent
duplicate likes, a forced-process-restart check preserving acknowledged writes, and reuse of the same
Puma socket after 66 idle seconds. A targeted concurrent-create/like check also passes: the action mutex
prevents the incorrect `already_liked: true` response possible when only individual queries were locked.

## Historical comparison at `9fafb32` with Rails PR #2 and the previous PR #12

The original Rails baseline is `ruby-rails-gambitboy` at `33dd92f6bc13d4b4a78e2ef3f50467fca2fc52cc`.
The previous version of this submission is commit `230b469` from PR #12.

The local comparison uses Ubuntu 24.04 ARM64 on Docker Desktop / Apple M2 Pro. Nginx and each Rails
application share one CPU and 2 GiB without container swap; the load generator runs on separate CPUs.
All three use the repository's Nginx site configuration and identical Ruby, Rails, Puma and SQLite gem
versions. Every trial starts with a fresh seed database on the container filesystem. Three trials per
workload and implementation use balanced execution order, `wrk -t2 -c64 --latency`, a two-second warmup
and a 15-second measurement. Mixed traffic follows the challenge's approximate read/like/create
proportions with live feed IDs and real writes, without think time.

| Metric | Rails PR #2 | Previous PR #12 | Optimized Rails |
|---|---:|---:|---:|
| Application lines | 216 | 149 | 158 |
| Nonblank, noncomment application lines | 172 | 132 | 141 |
| Application + runtime/build configuration lines | 497 | 188 | 197 |
| Feed requests/sec | 3,184 | 7,456 | 10,362 |
| Feed p99 | 25.92 ms | 12.22 ms | 13.24 ms |
| Single-post requests/sec | 3,707 | 9,732 | 13,860 |
| Single-post p99 | 46.42 ms | 9.72 ms | 14.15 ms |
| Mixed requests/sec | 3,551 | 7,980 | 11,583 |
| Mixed p99 | 29.71 ms | 17.20 ms | 16.09 ms |

Throughput and latency figures are medians of three runs, with 27 measured trials in total.
Both versions of PR #12 reported zero HTTP/socket errors. PR #2 reported 64 client timeouts in one
mixed trial; its result is retained in the medians. The cause of those timeouts was not established.
All 27 trials completed within the measurement-duration limits; none reported non-2xx/3xx responses.

Compared with the previous PR #12, throughput increases by **39% for feed**, **42% for single-post** and
**45% for mixed traffic**. Saturated read-only p99 increases, while mixed p99 decreases. Mixed summed
process RSS rises from 145.40 MiB to 185.94 MiB (PR #2: 216.34 MiB); these sums can count shared pages
more than once. Compared with PR #2, throughput is about **3.25× feed**, **3.74× single-post** and
**3.26× mixed**.

The optimizations add **nine application lines and no dependencies** to the previous PR #12, while
retaining **27% fewer application lines** than PR #2. Application source is counted conservatively:
PR #2's controllers, models and routes, excluding generated assets/views, versus this submission's
entire `app.rb`, including Rails bootstrap and routes. The runtime/build count also includes Rails,
Puma and Rack configuration, Gemfile and the three shell scripts. Both counts exclude lockfiles,
tests, documentation and third-party library source.

These local throughput measurements do not establish an official x86/k6 user-capacity score.

## License

MIT, under the repository's license.
