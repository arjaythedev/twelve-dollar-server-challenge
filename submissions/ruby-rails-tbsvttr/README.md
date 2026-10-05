# Ruby + Rails API + prepared SQLite

Rails 8.1.4, Ruby 4.0.5 with YJIT, Puma 8.0.2, sqlite3 2.9.6 and jwt 3.3.0.
All gem versions and checksums are pinned in `Gemfile.lock`; the Ruby source archive is checksum-pinned.

**Behind Nginx:** use the repository's `bench/nginx.conf`. Puma listens on `$HOST:$PORT`, with one request
thread and a 75-second keep-alive timeout. Nginx handles the many idle client connections.

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Implementation

This is a Rails application with `Rails::Application`, `ActionController::API`, Rails routing and controller
callbacks. It uses the SQLite driver directly instead of Active Record model objects. The full Rails gem
is pinned, but only the Action Controller railtie is loaded; assets, jobs, mailers and deployment tooling
are unnecessary for this API.

- Reused prepared statements execute against the live database on every request. SQLite constructs feed,
  post and create JSON, avoiding intermediate Ruby model objects, hashes and JSON serialization passes.
- One Puma request thread owns one SQLite connection and its prepared statements. No worker processes
  share that connection. WAL, `synchronous=NORMAL`, foreign keys, exclusive locking, a 500-page cache and
  a 512 MiB mmap limit are configured inside the process.
- `execute!` consumes each statement through `SQLITE_DONE`, and the cursor is reset before the response.
  In particular, an `INSERT ... RETURNING` is fully committed before returning 201.
- A like inserts only if the post exists, with conflict handling. A second lookup is needed only when
  nothing was inserted, to distinguish a duplicate like from a missing post.
- JWT signatures and expiration are checked on every authenticated request. No token verification,
  response, row or query-result cache is used.
- Rails' automatic JSON parameter parsing and wrapping are disabled. The create action parses the body
  after authentication, preserving the required error order. UTF-8, exact HS256, JavaScript whitespace
  trimming and the 500-code-point body limit are validated explicitly.
- ETag and conditional-response middleware are removed, because this API always returns current data.
- Production disables class reloading and static-file serving, so API requests do not probe the source
  or public directories for changed files. Logging is set to warning level.

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

## Comparison with Rails PR #2

The baseline is `ruby-rails-gambitboy` at `33dd92f6bc13d4b4a78e2ef3f50467fca2fc52cc`.
Application source is counted conservatively: its controllers, models and routes, excluding generated
assets/views, versus this submission's entire `app.rb`, including Rails bootstrap and routes. Test files,
documentation, lockfiles and third-party library source are excluded from that application count.

The local comparison uses Ubuntu 24.04 ARM64 on Docker Desktop / Apple M2 Pro. Nginx and each Rails
application share one CPU and 2 GiB without container swap; the load generator runs on separate CPUs.
Both use the repository's Nginx site configuration and identical Ruby, Rails, Puma and SQLite gem
versions. Every trial starts with a fresh seed database on the container filesystem. Three alternating
trials per workload use `wrk -t2 -c64 --latency`, a two-second warmup and a 15-second measurement.
Mixed traffic follows the challenge's approximate read/like/create proportions with live feed IDs and
real writes, without think time.

| Metric | Rails PR #2 | This Rails API |
|---|---:|---:|
| Application lines | 216 | **149** |
| Nonblank, noncomment application lines | 172 | **132** |
| Application + runtime/build configuration lines | 497 | **188** |
| Feed requests/sec | 3,391 | **6,730** |
| Single-post requests/sec | 3,696 | **9,127** |
| Mixed requests/sec | 3,223 | **7,375** |
| Mixed p99 | 34.86 ms | **24.56 ms** |

These are medians of three runs: about **2.0× feed**, **2.5× single-post** and **2.3× mixed** throughput,
with **31% fewer application lines**. The runtime/build count includes application source, Rails
configuration, Puma/Rack configuration, Gemfile and the three shell scripts for both implementations;
it excludes lockfiles, tests and documentation. All 18 measured trials reported zero HTTP/socket errors.
Both submissions passed the 42 official checks. This implementation also passed 70 extra HTTP checks,
16 concurrent duplicate likes, a forced-process-restart check preserving acknowledged writes, and
reuse of the same Puma socket after 66 idle seconds.

These local throughput measurements do not establish an official x86/k6 user-capacity score.

## License

MIT, under the repository's license.
