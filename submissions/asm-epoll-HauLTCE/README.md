# x86_64 asm (gas + epoll + sqlite3 + libcrypto)

single-file x86_64 linux assembly server (`server.s`, gnu `as` intel syntax). runs direct on `:80` with no nginx in front (or whatever `HOST` and `PORT` are set to).

## stack

- x86_64 assembly (`binutils` / `gcc` on ubuntu 24.04)
- raw `epoll_wait` / `accept4` / `read` / `write` / `fork` / `eventfd2` / `getrlimit` syscalls, non-blocking sockets
- `sqlite3` (`3.53.4` amalgamation compiled from sha256-verified source with `-O3 -march=native -DSQLITE_THREADSAFE=0`, or system `libsqlite3` fallback)
- `libcrypto` (`SHA256_Init` / `SHA256_Update` / `SHA256_Final` + `CRYPTO_memcmp`) for jwt signatures, claim checks (`alg`, `exp`, `sub`, `username`) in asm

## notes

- serves `:80` directly with no reverse proxy. before opening sqlite or accepting clients, worker 0 forks a sleeping standby child blocked on an `eventfd2` descriptor (`0%` cpu, empty fd table). below `63,500` connections (`rlim_cur - 1535`), worker 0 handles 100% of traffic solo with zero cross-process context switches or write-lock contention. the instant accepted fds reach `63,500`, worker 0 unregisters `listen_fd` from its `epoll` instance (keeping its `63,500` existing `keep-alive` sockets alive) and signals the standby worker via `eventfd2`, which forks the next standby worker and takes over `listen_fd`—scaling up to 6 cascaded workers (`381,000` persistent `keep-alive` sockets under `LimitNOFILE=65535`).
- every request reads live data from the unmodified sqlite schema with zero cross-request response or token caching. `GET /feed` scans the newest 20 ids on `posts_created_at_id_idx` (`2.7 µs`) and, when contiguous, does a range scan on `likes_post_id_idx` and `posts` primary key (`28 µs` total vs `70 µs` for 20 correlated subqueries), falling back to the standard query if sparse.
- jwt verification precomputes the 64-byte `ipad` and `opad` `SHA256_CTX` states from `JWT_SECRET` at startup and copies the 112-byte state (`rep movsq`) on each authenticated request to verify every token signature with `SHA256_Update` / `SHA256_Final` + `CRYPTO_memcmp` (`1.2 µs` vs `5.0 µs` for `HMAC(EVP_sha256())`).
- `GET` routes match 64-bit constants (`"GET /fee"`, `"GET /pos"`, `"GET /hea"`) in registers without scanning headers, and clean ascii `POST /posts` bodies (`{"body":"..."}`) are parsed and trimmed in registers before falling back to sqlite's `json_extract()`.
- reads in an `epoll_wait` batch share a `BEGIN ... COMMIT` snapshot. writes (`POST /posts` and `POST /posts/:id/like`) use `PRAGMA busy_timeout = 0` with non-blocking `SQLITE_BUSY` retry so an active worker never blocks its event loop waiting on a write lock, and `POST /posts/:id/like` checks `likes` and `posts` first so repeat likes and missing posts don't open unnecessary write transactions.