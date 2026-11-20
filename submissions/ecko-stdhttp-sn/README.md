# ecko-stdhttp-sn

The feed API from [SPEC.md](../../SPEC.md) in [Ecko](https://ecko.sh), using only its standard library.

| | |
|---|---|
| Language | Ecko v0.80.0 (pinned in `install.sh`) |
| Framework | `std.http`, Ecko's built-in HTTP/1.1 server |
| Driver | `std.sql`, SQLite compiled into the `ecko` binary |
| Serving | **Direct**, on `HOST:PORT` with no Nginx |

Ecko ships as one binary with SQLite inside it, so `install.sh` installs that
binary from the official download host and nothing else. There is no compile
step: `build.sh` runs `ecko check`, which refuses the program on any
error-severity finding before it starts.

## What it does

- One SQLite connection, opened at startup, in WAL mode with
  `synchronous = NORMAL`, the weakest setting rule 6 allows.
- `locking_mode = EXCLUSIVE`, set before WAL. Only this process opens the
  database, so it takes SQLite's lock once and keeps it, and the WAL index
  lives in process memory instead of a shared-memory file. Without it every
  read took and released a file lock: two `fcntl` calls per request, and about
  5% of the server's CPU in a local run. A 64 MB page cache (`cache_size`)
  goes with it.
- One query per read request, the reference query from SPEC.md. `like_count` is
  counted in that query, so it is current as of the request.
- Writes commit before the response is sent: `INSERT ... RETURNING` for a new
  post, `INSERT ... ON CONFLICT DO NOTHING` for a like, with the changed-row
  count telling a first like (201) from a repeat (200).
- Every request verifies its token's HS256 signature and `exp`. Nothing is
  cached between requests.
- The feed and single-post responses are built by SQLite: `json_object` keeps
  keys in the order the spec fixes, and `group_concat` joins the feed's 20
  posts, so the query returns the finished body as one string. Measured locally
  that is about 8% less CPU per request than building the same JSON in Ecko
  from 20 row maps (89 against 97 us at 40,000 users). The response to a new
  post is built in Ecko from the inserted row, with `json.encode` for strings.

## Why direct, and the two settings it changes

Going direct saves the CPU Nginx would use on the one shared core. The cost is
that every k6 user holds its own keep-alive connection, so `start.sh` changes
two defaults:

| Setting | Default | Here | Why |
|---|---|---|---|
| `ECKO_HTTP_MAX_CONNS` | 1,024 | 55,000 | most users keep their connection; see below |
| `ECKO_HTTP_READ_TIMEOUT_MS` | 30,000 | 75,000 | a k6 user is idle for up to 23 s between requests (`bench/load.js`), so its connection stays open for the next one |

Both can be overridden from the environment.

**Connections stay open, and the server recycles the idlest when it is
full.** Each open connection costs a file descriptor (the box allows 65,535)
and about 9 KB of memory on 0.80.0, so 55,000 of them peak near 500 MB
and leave the 2 GB box plenty of room. Raising the cap to 64,000 removed the
reconnect errors locally at 95,000 users (0.01% against 0.55%) but not the
latency, which is CPU-bound there, and it ran the process out of descriptors,
so it stays at 55,000. Past that, Ecko closes the connection that has been idle longest to make room for a
new user (since 0.77.0), and that user reconnects when it next asks for
something. Most requests therefore reuse a connection, which measured about a
quarter cheaper in CPU than reconnecting for every request (96 us against 126
us at 40,000 users locally).

Each connection is also a file descriptor. The box gives the service
`LimitNOFILE=65535`, but a login shell's soft limit is often 1,024, so
`start.sh` raises its own soft limit to 65,535 (or to the hard limit, if that
is lower). Without that, a local run from a terminal tops out near 1,000 users.

## Running it locally

From the repository root, with `seed/feed.db` built (`bash seed/make-seed.sh`):

```bash
bash test/run.sh submissions/ecko-stdhttp-sn                 # the 42 checks CI runs
bash submissions/ecko-stdhttp-sn/bench-local.sh 2500 2m      # one load probe at 2,500 users
```

`bench-local.sh` approximates the scoring run on one machine: the server is
pinned to one CPU (`SERVER_CPU`, default 3) and k6 gets the others (`K6_CPUS`,
default 4-31), spread over several loopback addresses so it is not capped by
one address's ephemeral ports. It prints pass or fail on the scoring thresholds
(p95 < 500 ms, p99 < 1 s, errors < 1%), the request rate, and the server's CPU
time and peak memory, and appends that line to `bench-out/results.txt`. It is
not the real droplet: memory is not capped and the CPU is this machine's.

Both scripts use the `ecko` on `PATH`; set `ECKO=/path/to/ecko` to try another
build, and `K6=/path/to/k6` likewise.
