# zsh-ztcp-unorig-username

The web server is the Z shell. One `zsh` process listens with `ztcp -l`, accepts with `ztcp -a`, waits on
every socket with `zselect`, reads and writes with `sysread` and `syswrite`, and computes HMAC-SHA256 in
`$(( ))`. No socat, no `nc`, no subshells, no other process. The shell that came with your login prompt
now serves a JSON API. Nobody asked for this.

- **Language:** zsh script (`server.zsh`, about 240 lines)
- **Runtime:** zsh 5.9, built from the release tarball by `build.sh` (sha256 checked)
- **Network:** zsh's own modules: `zsh/net/tcp` (`ztcp`), `zsh/zselect`, `zsh/system`
- **SQLite:** `zsh/sqlite`, a zsh module of about 140 lines of C (`module/sqlite.c`) that is compiled into
  this zsh build and linked against Ubuntu's `libsqlite3` (3.45.1, `libsqlite3-dev`). It is the driver:
  `zsqlite SQL ARGS...` binds the arguments, steps the statement and returns the rows in `$reply`. In-process,
  one connection, prepared statements reused by SQL text.
- **JWT:** HMAC-SHA256 written in zsh arithmetic; the claims are checked with SQLite's JSON functions
- **Mode:** **behind Nginx** (`bench/nginx.conf`, the server on port 3000). Not direct: `zselect` is
  `select(2)`, which stops at 1,024 descriptors, and ~15,000 direct connections are far past that.

## How a request goes

1. `zselect -r` waits on the listening socket and every open connection. Ready connections are read with
   `sysread`, then the listening socket is accepted with `ztcp -a`.
2. Each connection has a buffer. Every complete request in it is answered in order (pipelining and requests
   split across reads both work): the head ends at the first blank line and the body is `Content-Length`
   bytes. Headers are found with zsh's extended globbing, case-insensitively (`(#i)authorization:`).
3. Routing is one `case` on `"$method $path"` (the query string is dropped), with glob patterns such as
   `'GET /posts/'[^/]##`.
4. The SQL writes the JSON: `json_object()` produces every post, so escaping is SQLite's job. The feed is a
   `group_concat` over the 20 newest posts. A create is `INSERT ... RETURNING json_object(...)`. A like is
   `INSERT ... SELECT ... ON CONFLICT DO NOTHING RETURNING post_id`, and a lookup only if nothing was inserted.
5. The response goes out with one `syswrite` per read (the answers to all pipelined requests together).

`POST /posts` bodies are checked by SQLite: `json_valid()` (RFC 8259 strict in 3.45), then `json_each()` for
the last `body` key, `trim()` of space, tab, CR, LF, VT and FF, and `length()` in characters.

## HMAC-SHA256 in shell arithmetic

zsh has 64-bit integers and C's bitwise operators, so SHA-256 is a function of `(( ))` expressions:
the message schedule and the 64 rounds over words held in 64-bit integers and masked to 32 bits, rotations as
`x>>n | x<<(32-n)`. The JWT's `header.payload` is turned into bytes with `${(s::)…}` and `#c` (a
character's code), padded, and compressed block by block. The key's ipad and opad blocks are compressed
once at start (a key schedule; every token still gets its own full HMAC). The tag is base64url-encoded in
zsh and compared with the token's signature. A token costs a few milliseconds of shell arithmetic (about 2 ms on a laptop core). `server.zsh
selftest` prints the HMAC of `abc`, to compare with `openssl dgst -sha256 -hmac`.

Then the claims: `header` and `payload` are base64url-decoded in zsh and handed to one SQL statement that
checks `alg` = `HS256`, `exp` (required, a number, in the future), `nbf` (if present), `sub` (a string of
digits, positive, under 19 digits) and `username` (a string), and picks the spec's 401 message.

## The rules

- **SQLite in-process (rule 2):** the database is opened inside the zsh process by the `zsh/sqlite` module,
  not through a `sqlite3` CLI or any other process.
- **No caching (rule 5):** every request runs its query; only prepared statements are kept.
- **Durability (rule 6):** WAL, `synchronous=NORMAL`. Each write is its own autocommit statement, so the 201
  is written after its commit.
- **Changes to zsh:** `build.sh` adds the `zsh/sqlite` module and changes three lines of `ztcp -l`
  (`Src/Modules/tcp.c`): the listen backlog goes from 1 to 4096, the socket gets `SO_REUSEADDR` so a
  restart can bind while old connections are in `TIME_WAIT`, and it binds to `$HOST` instead of always
  `0.0.0.0`, so behind Nginx port 3000 is not reachable from outside. The rest is stock zsh 5.9.

## Limits (shortcuts)

- `HOST` must be an IPv4 address (`127.0.0.1` or `0.0.0.0`), not a hostname.
- Connections whose descriptor would be 1,000 or more are closed at once (`select(2)` cannot watch them).
  Behind Nginx there are about 64.
- `syswrite` blocks: a client that pipelines a lot without reading stalls the server. Nginx always reads.
- No `Transfer-Encoding: chunked`, no `Expect: 100-continue`. Requests over 1 MB close the connection.
- A `JWT_SECRET` longer than 64 bytes is refused at start (the key would have to be hashed first).

## Numbers (local, rough)

Not the droplet: an x86_64 Ubuntu 24.04 container limited to 1 CPU, emulated on an ARM laptop that was busy
with other work, with `bench/nginx.conf` in the same container and k6 in another one. `bench/load.js`, 60 s
ramp, 2 min hold, one run each. Local runs like this have read 1.3 to 2.5 times higher than the real box.

| k6 users | req/s | p95 | p99 | failed | CPU (zsh + Nginx) | zsh RSS |
|---|---|---|---|---|---|---|
| 5,000 | 415 | 5.3 ms | 9.0 ms | 0% | | 27 MB |
| 15,000 | 1,246 | 17 ms | 33 ms | 0% | 88% | 49 MB |
| 18,000 | 1,446 | 81 ms | 10.8 s | 0% | 100% (saturated) | 54 MB |

`test/run.sh`: 42 passed, 0 failed.
