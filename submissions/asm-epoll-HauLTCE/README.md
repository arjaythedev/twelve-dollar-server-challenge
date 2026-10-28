# x86_64 asm (gas + epoll + sqlite3 + libcrypto)

single-file x86_64 linux assembly server (`server.s`, gnu `as` intel syntax). runs direct on `:80` with no nginx in front (or whatever `HOST` and `PORT` are set to).

## stack

- x86_64 assembly (`binutils` / `gcc` on ubuntu 24.04)
- raw `epoll_wait` / `accept4` / `read` / `write` syscalls, single thread, non-blocking sockets
- `libsqlite3` (`3.45.1` from apt)
- `libcrypto` (`HMAC` + `EVP_sha256` + `CRYPTO_memcmp`) for jwt signatures, claim checks (`alg`, `exp`, `sub`, `username`) in asm

## notes

- serves `:80` directly so we don't burn cpu on nginx. keeps connections alive up to fd `65000` and sends `Connection: close` past that so `LimitNOFILE=65535` doesn't kill us with `EMFILE`.
- routes are matched with 32-bit register compares (`"GET "`, `"/fee"`, `"/pos"`, etc.). sqlite statements return raw columns and we write the json straight into `.bss` buffers in asm instead of calling `json_object()` or `snprintf()`.
- when `epoll_wait` returns a batch of events, reads share a single `BEGIN ... COMMIT` snapshot, and all writes (`POST /posts` and `POST /posts/:id/like`) in that tick get committed together in one WAL transaction before sending `201` / `200`.
- sqlite runs with `WAL`, `synchronous = NORMAL`, `mmap_size = 1GB`, `cache_size = -16000`, and `wal_autocheckpoint = 0` (we run `PRAGMA wal_checkpoint(PASSIVE)` every 64 write batches after sending responses). startup runs `SELECT count(*)` over the indexes to fault the mmap pages in before k6 starts.