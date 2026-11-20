# Go + net/http + SQLite (baseline)

A deliberately plain baseline. It uses the standard library HTTP server and JSON encoder, and the most common Go SQLite driver.

| | |
|---|---|
| Language | Go 1.27.1 (installed from the official tarball, sha256-pinned in `install.sh`) |
| HTTP | `net/http` |
| SQLite driver | `github.com/mattn/go-sqlite3` v1.14.48 (cgo, bundled SQLite) |
| Front | **Nginx** (`bench/nginx.conf`, `127.0.0.1:3000`) |

## Design

- **One writer, a few readers.** Writes go through a single `database/sql` connection, because SQLite allows one writer at a time and a single connection serializes writes in-process instead of retrying on `SQLITE_BUSY`. Reads use a separate pool of 4 connections.
- **WAL with `synchronous=NORMAL`**, the minimum rule 6 allows. Each write is its own transaction, committed before the 201 is sent. There's no group commit yet.
- **Prepared statements** for all six queries, prepared once at startup.
- **No caching** of any kind. Every request queries SQLite, and every token's HS256 signature and `exp` are verified.
- JWT verification is hand-rolled on `crypto/hmac`, about 30 lines, with no dependency.
- `IdleTimeout` is 120 s, so Nginx's upstream keep-alive sockets are never closed under it (the spec asks for at least 65 s).

## Optimizations

None yet, on purpose. This is the starting point for later PRs, which will measure each change one at a time.
