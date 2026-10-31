# Go net/http + SQLite

**Language:** Go 1.27.1
**Framework:** standard library `net/http`
**Driver:** `github.com/mattn/go-sqlite3` v1.14.48 (cgo)
**Serving:** direct on `HOST:PORT` (`0.0.0.0:80` in the benchmark); no Nginx.

This is the baseline submission: plain `net/http` routing, one SQL statement per request, no
response caching of any kind. Reads come from SQLite on every request.

## Implementation notes

- `database/sql` pool of 8 connections, each opened with `_busy_timeout=5000`,
  `_journal_mode=WAL` and `_synchronous=NORMAL`, so writes are committed before the 201 is sent.
- One query per request, matching the reference queries in `SPEC.md`; `like_count` is counted
  at request time.
- JWT HS256 verification is implemented directly with `crypto/hmac`; only `HS256` is accepted
  and `exp` is checked on every request (no token cache).
- JSON is encoded from structs so key order is fixed, with HTML escaping disabled.
- Keep-alive is Go's default; no idle timeout is set, so idle sockets stay open far beyond the
  65 s Nginx requires when running behind it.

## Scripts

- `install.sh` — Go 1.27.1 toolchain and `build-essential` (cgo).
- `build.sh` — pinned module download, verify, and a trimpath stripped build to `bin/server`.
- `start.sh` — runs `bin/server` in the foreground.
