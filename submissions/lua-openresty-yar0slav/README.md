# Lua + OpenResty: no app server was harmed in the making of this submission

I'm probably late to the party: I learned about the challenge from the follow-up video, which already
had contestants who actually maxed out the default Ubuntu box. Still, I noticed a gap I wanted to fill.

Everybody's throwing nginx away. But nginx is a great piece of software, so as an SRE I decided to go
against the trend and drop the *app server* instead, keeping the most stable software around for the
critical job: **nginx and SQLite**. No cutting corners, no half-parsing requests against the expected
pattern, the full network stack available. My generation didn't have Rust and AI. We had C to shoot
our feet.

So yes, the "framework" is `nginx.conf`. The "app" is ~300 lines of Lua that nginx runs for every request.

> The design is very human and it's easy to use(c)

## What's in the box

| | |
|---|---|
| Language | Lua 5.1 (LuaJIT 2.1, bundled with OpenResty) |
| Framework | OpenResty 1.31.1.1 (nginx + lua-nginx-module), official OpenResty apt repo, pinned to `1.31.1.1-1~noble1` |
| SQLite driver | none, on purpose: a dozen SQLite C functions declared through LuaJIT's FFI, calling Ubuntu's `libsqlite3-0` (SQLite 3.45.1) |
| JWT | hand-rolled HS256: OpenSSL's `HMAC()` through the FFI (OpenSSL 3 ships with OpenResty) |
| JSON | `lua-cjson` (bundled with OpenResty), for parsing and string escaping only |
| **Nginx or direct** | **Direct**, and also **Nginx**. OpenResty *is* nginx, serving `0.0.0.0:80` itself. Putting the benchmark's nginx in front of it would be nginx proxying to nginx, which is funny but not fast. |

There are no third-party Lua packages to download, so `build.sh` has nothing to build. It just
syntax-checks the Lua, which is the most a build script has ever done for me without complaining.

## Running it

```bash
sudo bash install.sh   # apt: openresty (pinned) + libsqlite3-0
bash build.sh          # byte-compiles lua/*.lua as a check
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`start.sh` renders `nginx.conf` into `run/` with `HOST:PORT` filled in and runs OpenResty in the
foreground with `run/` as its prefix, so the pid file and temp dirs are writable by a non-root user.
Port 80 works through the service's `CAP_NET_BIND_SERVICE`.

## How it works, and why

- **The HTTP server is nginx.** Parsing, keep-alive and connection handling are nginx's C code on
  epoll, the same code that holds up a good chunk of the internet. Idle keep-alive connections cost a
  few hundred bytes each: 60k connected users used ~450 MB of RAM *in total*.
- **One worker process.** One vCPU, so one event loop. SQLite calls block the worker, but every
  query is an indexed lookup on a warm page cache: microseconds of CPU-bound work, which async I/O
  wouldn't make any faster on one core. A single writer also means no SQLite lock contention, ever.
- **Connection limits sized for direct mode:** `worker_rlimit_nofile 65535` (the service's
  `LimitNOFILE`), `worker_connections 65000`, `keepalive_timeout 75s`, `keepalive_requests 1000000`,
  `listen ... backlog=4096`, and `access_log off`, because writing thousands of log lines a second is
  a hobby, not a requirement.
- **SQLite through the FFI**, opened in `init_worker_by_lua` (handles never cross nginx's fork), with
  every statement prepared once (`SQLITE_PREPARE_PERSISTENT`) and reused. Pragmas:
  `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 64 MiB page cache, 1 GiB `mmap_size`.
- **SQL:** the reference post query (likes counted per request via `likes_post_id_idx`). A new post
  is inserted with `RETURNING id, created_at`, and the statement is stepped to completion before the
  response goes out, so the autocommit transaction has committed. A like is a single
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`, and only when it inserts nothing
  does a second lookup tell "already liked" from "no such post".
- **JSON built by string concatenation.** That's how you get the spec's exact key order (Lua tables
  have no order and no shame), and it skips encoding the whole response. Only user text (`body`,
  `author`) and error strings go through `cjson.encode`, with forward-slash escaping turned off.
- **JWT verified on every request** (rule 5): split, HMAC-SHA256 over `header.payload`, compare with
  the token's signature, then check `alg == "HS256"`, `exp` (and `nbf` if present), `sub` and
  `username`, which map to the spec's three distinct 401 messages. The HMAC costs about 0.4% of the
  server's CPU, so there was never any temptation to cache it.
- **Bodies stay in memory:** `client_body_buffer_size` equals `client_max_body_size` (16 KB), so nginx
  never spools a request body to a temp file.
- **Body length in characters, not bytes:** Lua's `#` counts bytes, so bodies over 500 bytes count
  their UTF-8 codepoints. `café ✓` is 6 characters, whatever Lua thinks.

### Things I tried that didn't help (measured, then reverted)

- **SQLite `locking_mode=EXCLUSIVE`** (skip the file lock on every read). Droplet A/B at 40k users,
  2 rounds each: 61.8% vs 60.7% busy CPU, well inside the noise. Not worth making the database
  unreadable to everything else while the server runs.
- **Building the post and feed JSON inside SQLite** with `json_object()`, so `/feed` comes back as one
  string. Local A/B in a 1-CPU container, 2,000 req/s in `load.js`'s mix, 3 rounds: 119.6 µs vs
  123.2 µs of CPU per request. SQLite's JSON was **3% slower** than Lua gluing strings together.
  LuaJIT stays undefeated.

### Where the CPU actually goes

`perf` on the droplet with 15k users connected, as a share of the server's busy time:

| Part | Share |
|---|---|
| Kernel networking (TCP, syscalls, the virtual NIC) | ~45% |
| SQLite | ~22% |
| LuaJIT + cjson + OpenSSL | ~16% |
| nginx | ~13% |
| libc | ~6% |

Nearly half the CPU goes to the kernel moving packets, and rule 11 says hands off the kernel. The app
is already cheap (~120 µs of CPU per request, all in), so the remaining headroom is mostly physics
and neighbours.

## Load test results

Run on DigitalOcean, replicating the follow-up video's setup with **4 load-generating servers**:

- **Server:** `s-1vcpu-2gb` (the $12 box), Ubuntu 24.04, run as a systemd service the way the
  benchmark does: non-root user, `LimitNOFILE=65535`, `CAP_NET_BIND_SERVICE`, no kernel tuning, and a
  fresh copy of `seed/feed.db` for every run.
- **Load:** 4 × `s-4vcpu-8gb` in the same region, k6 v2.3.0 running the repo's `bench/load.js`, users
  split evenly across them, over the private network. (The smaller runs early on used 2–3 of them;
  the table says which. At these sizes the server didn't notice.)
- **Runs:** 60 s ramp, 90 s hold, 30 s ramp-down; pass = p95 < 500 ms, p99 < 1 s, < 1% errors.

| Users | Load gens | Requests | Errors | p95 | p99 | Server CPU | Result |
|---|---|---|---|---|---|---|---|
| 20,000 | 2 | 302,477 | 0 | 9 ms | 21 ms | 40% | PASS |
| 30,000 | 2 | 454,121 | 0 | 16 ms | 54 ms | 49% | PASS |
| 40,000 | 4 | 605,356 | 0 | 8 ms | 25 ms | 60% | PASS |
| 48,000 | 3 | 725,507 | 0 | 42 ms | 206 ms | 72% | PASS |
| 56,000 | 4 | 846,982 | 0 | 20 ms | 105 ms | 79% | PASS |
| 59,500 | 4 | 899,702 | 0 | 24 ms | 126 ms | 80% | PASS |
| 59,600 | 4 | 900,803 | 0 | 32 ms | 130 ms | 80% | PASS |
| 59,700 | 4 | 883,702 | 0 | 1.18 s | 4.04 s | 81% + 9% steal | FAIL |
| 60,500 | 4 | 910,260 | 0 | 553 ms | 803 ms | 82% | FAIL |

**5-minute hold at 59,600 users: PASS.** 2,223,328 requests, **0 errors**, p95 74 ms, p99 388 ms.

Zero errors in every run, including the failed ones. Past ~59.5k it doesn't break, it just gets
slow: the single vCPU is full and requests start queueing.

Now, about those neighbours. Near the limit, the result depends on them a lot. A Basic droplet's vCPU
is shared, and the hypervisor took anywhere from 2% to 16% of it ("steal") depending on the run and
the hour. 59,700 users failed with 9% steal while 60,500 nearly passed with 3%. So read the limit as
**~60k users, give or take a thousand**, depending on who else lives on the host that day.

And that neighbour noise moves the result quite a lot. At ~60k the server is CPU-bound with the
neighbours taking their cut. Give it a quiet host, and the remaining few thousand users up to the
`LimitNOFILE` ceiling (~65k connections) are within reach. On a good day, nginx and SQLite might just
max out the box, no app server required.

## License

MIT, under the repo's [license](../../LICENSE).
