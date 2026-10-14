# Ruby + Async

| | |
|---|---|
| Language | Ruby 4.0.7 with YJIT, built from source with jemalloc and `-march=native` |
| Framework | [async](https://github.com/socketry/async) 2.46.0 reactor on io-event 1.22.1 (io_uring selector, epoll fallback); the HTTP/1.1 layer is our own (`http.rb`, `epoll.rb`) |
| SQLite driver | extralite 3.1.3, statically linked to SQLite 3.54.0, built from the amalgamation with profile-guided optimization |
| JWT | HMAC-SHA256 from libsodium 1.0.22, called through rbnacl 7.1.2's FFI bindings |
| JSON | built by SQLite (`json_object`); json 3.1.0 parses request bodies and JWT claims |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

Ruby, SQLite and libsodium are downloaded at pinned versions and checked against their SHA-256 sums.
Every gem, transitive ones included, is pinned with checksums in `Gemfile.lock`.

## Running it

```bash
sudo bash install.sh   # apt build deps, then Ruby, SQLite and libsodium from source (~10 min on 1 vCPU)
bash build.sh          # gems into vendor/bundle
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

The first version ran on Falcon. Under load, its fiber per connection and the objects async-http
keeps for each one cost about 40 KB per connection, and garbage collection had to scan all of that.
At 20k connections each GC paused for about 150 ms, and that pause set the latency floor. This
version keeps the Async reactor and replaces the HTTP server.

- **Idle connections own no fiber.** An idle keep-alive connection sits in a kernel epoll set
  (one-shot) and owns no Ruby fiber. One Async task waits on that epoll descriptor through the reactor.
  A pool of 256 worker fibers reads each ready connection, answers every complete request in its
  buffer in order, writes each response with one `write`, then re-arms the connection. Memory is
  about 5 KB per connection (120 MB at 20k, 175 MB at 40k), and GC drops from about 4 s to about
  0.15 s per minute.
- **A minimal HTTP/1.1 parser.** It reads the request line and the four headers the API uses:
  `Content-Length`, `Transfer-Encoding`, `Connection` and `Authorization`. It supports keep-alive,
  pipelining, partial reads, HTTP/1.0 and `Connection: close`. Chunked request bodies get a 411.
  Response heads are prebuilt frozen strings.
- **io_uring.** io-event picks io_uring on the droplet's kernel, which cut kernel CPU roughly in half
  compared with epoll in our tests.
- **SQLite builds every response.** `json_object` and `group_concat` produce the exact response bytes,
  so Ruby never allocates a hash per row or serializes JSON on reads. All statements are prepared once.
- **Group commit.** The first write in a reactor pass opens `BEGIN IMMEDIATE`, every write in that pass
  joins the transaction, and one `COMMIT` runs on the next pass. No write response goes out before its
  `COMMIT` returns, and reads that ran while a batch was open wait for it too. That gives one WAL
  append per batch instead of one per write.
- **Pragmas**: `locking_mode=EXCLUSIVE`, so the WAL index lives in heap memory rather than the `-shm`
  file; `journal_mode=WAL`; `synchronous=NORMAL` (rule 6); `wal_autocheckpoint=0`, with a passive
  checkpoint every second on a timer, off the commit path; 1 GiB `mmap_size`; 64 MiB page cache.
- **SQLite compiled for this workload.** It's a static library built with sqlite.org's recommended
  options (`DQS=0`, `DEFAULT_MEMSTATUS=0`, `OMIT_DEPRECATED`, `OMIT_SHARED_CACHE`, `USE_ALLOCA`, and so
  on). It's then profile-guided: `sqlite_train.c` runs the server's own statements in the load test's
  mix against a synthetic database, and the final build uses that profile. That made reads about 7%
  faster.
- **One thread, and SQLite never releases the GVL.** Every query is an index lookup that takes
  microseconds, so releasing and reacquiring the GVL would cost more than it saves.
- **JWT verified on every request**, with no caching (rule 5). libsodium's one-shot HMAC-SHA256 takes
  1.6 µs, compared with 3.5 µs through OpenSSL's HMAC in Ruby, and signatures are compared in
  constant time. The standard `{"alg":"HS256","typ":"JWT"}` header is compared as a string, and any
  other header is parsed and must say `HS256`. `exp`, `nbf`, `sub` and `username` are checked.
- **Likes in one statement**: `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`. A
  post-existence check runs only when nothing was inserted, to tell "already liked" from "no such post".
- **Direct instead of Nginx.** On one core, Nginx's CPU comes out of Ruby's budget, and the provided
  `nginx.conf` caps out near 16k connections. Responses switch to `Connection: close` only within 256
  descriptors of the process's file limit, so clients aren't forced to reconnect under normal load.

## Results on a 1-vCPU test VM

These ran on a Proxmox VM: 1 dedicated Zen 3 vCPU, 2 GB RAM, Ubuntu 24.04, io_uring. Load came from a
separate 8-vCPU VM. That vCPU is roughly twice as fast as the droplet's, so treat these as relative
numbers. Iodine (PR #14), which scored 14k on the droplet, ran on the same VM for comparison.

| Load | This server | Iodine #14, same VM |
|---|---|---|
| k6 `bench/load.js`, 30k VUs | p95 2.1 ms, p99 7.9 ms, 0% failed | — |
| 25k users (C load generator) | 41% CPU, 153 MB, p95 5 ms | 49% CPU, 662 MB, p95 21 ms |
| 40k users (C load generator) | 75% CPU, 174 MB, p95 14 ms, p99 412 ms | 87% CPU, 990 MB, p95 138 ms, p99 784 ms |

Test suite: 42/42 on x86_64.

## Trade-offs

- Linux only, because epoll is bound through FFI.
- No idle timeout. Connections stay open until the client closes them, and the descriptor guard
  covers the extreme case.
- The parser covers what this API needs: no chunked request bodies and no `Expect: 100-continue`.

## License

MIT, under the repo's [license](../../LICENSE).
