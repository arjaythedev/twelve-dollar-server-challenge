# TypeScript + Deno

A small feed API, a stripped-down Twitter, built to be as fast as humanly
possible on the [Deno](https://deno.com) runtime. It is an entry in the **$12
Server Challenge**: how many users can one $12/month server handle?

|                     |                                                                                                                 |
| ------------------- | --------------------------------------------------------------------------------------------------------------- |
| Language            | TypeScript on Deno 2.9.7 (pinned in `install.sh`)                                                               |
| Framework           | None: raw TCP and a hand-written HTTP/1.1 parser (`server.ts`)                                                  |
| Network layer       | Linux: `poller.rs`, an epoll loop built with Ubuntu 24.04's `rustc`, no crates. macOS (dev only): `Deno.listen` |
| SQLite              | 3.53.2, downloaded and built from source by `build.sh` with PGO, called over Deno FFI (`sqlite.ts`)             |
| JSON                | Rendered in C from the rows (`render.c`), byte-identical to SQLite's `json_object`                              |
| JWT                 | `node:crypto` HMAC, verified by hand on every request                                                           |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself                                                                          |

## The challenge

Every entry implements the same API against the same SQLite database, then gets
benchmarked on the same box: a DigitalOcean droplet with **1 vCPU and 2 GB of
RAM**, running Ubuntu 24.04. A load test (k6, from a separate machine) ramps up
simulated users, each one reading the feed, opening posts, liking and posting.

The score is the **most concurrent users the server holds for 5 minutes** with
p95 latency under 500 ms, p99 under 1 s and under 1% errors. Everything runs on
one server with one SQLite file and no caches in front of it: every request
reads from the database while it's being served, and every write is committed
before the response goes out.

## The API

JSON in and out, compact, keys in a fixed order. The full contract is
[`SPEC.md`](../../SPEC.md); the 42-check test suite,
[`test/test.sh`](../../test/test.sh), decides any disagreement.

| Request                       | Success                        | Body                                                                         |
| ----------------------------- | ------------------------------ | ---------------------------------------------------------------------------- |
| `GET /health`                 | 200                            | `{"status":"ok","db":"ok","uptime_s":<int>}`                                 |
| `GET /feed`                   | 200                            | `{"posts":[<post> × 20]}`, the 20 newest posts                               |
| `GET /posts/:id`              | 200                            | `{"post":<post>}`                                                            |
| `POST /posts` (auth)          | 201                            | `{"post":<post>}` for the new post. Request body: `{"body":"..."}`           |
| `POST /posts/:id/like` (auth) | 201 first time, 200 on repeats | `{"liked":true,"already_liked":<bool>,"post_id":<int>}`, one per user + post |

A post is
`{"id":<int>,"body":<string>,"created_at":<string>,"author":<string>,"like_count":<int>}`,
with the like count as of that request. Errors are `{"error":"<message>"}` with
a 400, 401, 404 or 500.

## Running it

```bash
sudo bash install.sh   # once, as root, on a clean Ubuntu 24.04: Deno 2.9.7, build-essential, rustc
bash build.sh          # SQLite + render.c with PGO into bin/, poller.rs, type check
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

`deno task test` runs `render.test.ts` and then the challenge's `test/run.sh`
(build, start on a fresh seed copy, the 42 checks). `bin/` and `vendor/` are
build output and gitignored.

## Optimizations, and why

The box has one vCPU, so every optimization is about CPU per request and about
not paying for 15,000 or more idle keep-alive connections.

- **Direct, one process, one request thread.** Nginx would take its CPU out of
  the same core. On Linux the JS thread blocks in `epoll_wait` inside
  `poller.rs` (a plain synchronous FFI call), so a request never goes through
  Deno's event loop, ops or promises. Rust owns the sockets and copies each
  ready connection's bytes into one JS-owned buffer per wake; JS parses, answers
  and reports what it consumed. An idle connection costs about 7 KB against
  `Deno.serve`'s 19 KB, and 64,000 open connections fit in about 230 MB of RSS.
- **HTTP/1.1 by hand.** The parser reads only what the challenge sends: request
  line, `Authorization`, `Content-Length`, fixed-length bodies, keep-alive,
  pipelining. Each response is one buffer and one `write`.
- **Past the fd limit.** `LimitNOFILE=65535` caps open sockets, and batch 2
  showed the cost: users past ~65,500 waited in the listen queue until they
  timed out. Now, once open connections pass the limit minus 1,024, responses
  carry `connection: close` and the socket closes after the write, so those
  users reconnect per request instead (about 28 µs of CPU each on a droplet).
- **SQLite built for this box.** `build.sh` downloads SQLite 3.53.2 (sha256
  pinned), compiles it `-O3 -march=native` with `THREADSAFE=2`, `MEMSTATUS=0`
  and the usual recommended omits, then recompiles with profile-guided
  optimization from a training run of the k6 request mix. `sqlite.ts` binds only
  the dozen calls the server makes; every statement is prepared once with
  `SQLITE_PREPARE_PERSISTENT`.
- **JSON rendered in C.** `json_object` + `group_concat` was 41% of the feed
  query. `render.c` steps the rows and writes `{"posts":[...]}` straight into a
  buffer with SQLite's own escape table, one FFI call, so the body never becomes
  a JS string. `render.test.ts` checks it against `json_object` byte for byte.
- **Durability: `synchronous = FULL`**, stronger than rule 6 asks, in WAL mode.
  Each connection keeps one transaction open (autocommit pays setup per
  statement). On Linux the writes in one epoll batch share a COMMIT (group
  commit, rule 6 allows it), and the COMMIT itself, the fsync, runs on a C
  thread in `render.c` while the request thread serves reads. Every write's
  response is held until its COMMIT lands; an eventfd wakes the loop. A 2–3 s
  disk stall, which this droplet type does have, delays writes only.
- **Two connections.** `db` writes and also reads while it is free (its own
  commits never reset its page cache). While a COMMIT is in flight, reads go to
  `rdb`, whose snapshot is refreshed at most every 20 ms after a commit lands:
  SQLite drops a connection's whole page cache when it reopens on a WAL another
  connection changed, so refreshing per commit cost 37 µs per request.
- **Checkpoints off the request thread.** A C thread checkpoints every 100 ms:
  PASSIVE, or TRUNCATE once the WAL passes 4,096 frames, because reads walk
  every WAL hash segment and a 60K-frame WAL made the feed ten times slower.
  While a TRUNCATE waits for readers the request thread drops its idle snapshot
  and refreshes `rdb` each batch, so the wait drains and the lock is never
  contended.
- **JWT by hand**: HS256 only, constant-time signature compare, `exp` checked,
  on every request (rule 5: nothing is remembered between requests).
- **No caching anywhere.** Every request runs its query. SQLite's page cache (64
  MiB) and a 1 GiB `mmap_size` are the only memory between requests.
- **Process settings** (rule 11 allows in-process settings): `start.sh` raises
  the fd soft limit to the hard limit, runs V8 with `--single-threaded-gc` so no
  GC helper thread competes for the vCPU (p99 1,122 → 750 ms at 40K users), sets
  `oom_score_adj` so the kernel kills the server before `sshd`, and restarts it
  after a crash or SIGKILL.

## Measured

On an `s-1vcpu-2gb` droplet with a separate load generator (kload, a
k6-compatible generator, 64,000 users, 5-minute holds) the hold passed at about
65% CPU with p95 under 65 ms and p99 under 400 ms, including a run that hit a
2.6 s disk stall. Past the fd limit, 72,000 users passed a 5-minute hold with
0 errors at about 90% CPU (p95 259 ms, p99 473 ms; ~11% of requests
reconnected); 80,000 failed on CPU, not errors. That droplet is about 1.7×
slower per request than the batch 2 box. The organizer's score will come from
their own load generator on their own box.

The measurement scripts (wrk per endpoint, connection storms, the droplet
harness) live in the source repository,
[JakeAve/twelve-dollar-server-deno](https://github.com/JakeAve/twelve-dollar-server-deno).

## License

MIT, under the repository [license](../../LICENSE).
