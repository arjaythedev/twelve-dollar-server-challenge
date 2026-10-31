# SQL, served by the sqlite3 CLI (Bash only moves bytes)

The web application is written in SQL and run by the `sqlite3` command-line shell. SQLite parses
the HTTP request, routes it, verifies the JWT with HMAC-SHA256 written as a recursive CTE, runs the
queries and renders the whole HTTP response. Bash listens on the socket itself and moves bytes,
because `sqlite3` can't open a socket. No Nginx, no socat, no compiled code of our own.

- **Language:** SQL (SQLite 3.45.1 dialect), plus ~80 lines of Bash 5.2 that move bytes
- **Runtime:** the stock `sqlite3` CLI and Bash's `accept` loadable builtin (Ubuntu 24.04 packages `sqlite3`, `bash-builtins`)
- **Framework, driver:** none; the database is the CLI's own
- **Nginx or direct:** **direct**, on `0.0.0.0:80`

## How a request flows

1. **Bash** (`server.sh`) accepts the connection with `enable -f accept accept`, reads the request
   head line by line and the body by `Content-Length` (`read -N`), and writes this to a file:

   ```sql
   REPLACE INTO temp.req VALUES(1, '<raw head>', '<raw body>');   -- quotes doubled
   .once <plan>
   SELECT '.read route/' || route || '.sql' FROM temp.q;
   .read <plan>
   .once <this connection's FIFO>
   SELECT octet_length(r) || char(10) || r FROM temp.http;
   ```

   Then it sends one line, `.read <that file>`, to a sqlite3 process.
2. **The HTTP parser is a trigger.** Inserting into `temp.req` fills `temp.q` (method, path,
   Authorization, post id, route, the token's three parts) one `UPDATE` stage at a time.
3. **The router is a `SELECT` that writes a `.read` command into a file**, which the CLI then
   reads. That's `eval` for the sqlite3 shell, and it means only write routes ever take the write lock.
4. **Routes** are plain SQL files in `sql/route/`. Like and create run `auth.sql` first.
5. **`temp.http`** turns the route's status and body into the full response, with
   `Content-Length` from `octet_length()`. If a route fails, it falls back to a 500, or to a 503 on `/health`.

## The JWT check, in SQL

SQLite has no SHA-256, no XOR and no user-defined functions, so:

- **SHA-256 is a recursive CTE** with one row per round: the 8 working variables, the 8 chaining
  values and a 16-word message window are columns. XOR is `a+b+c-2(a&b+a&c+b&c)+4(a&b&c)`.
- **SQL writes that SQL.** `build.sh` runs `sql/gen-hmac.sql`, which expands the round
  expressions from text templates with `replace()` and prints `sql/hmac.sql`.
- **No constants are typed in.** The 64 round constants and 8 initial hash values are the
  fractional parts of `pow(p, 1.0/3)` and `sqrt(p)` over primes from a recursive-CTE sieve.
- **The key blocks are hashed once per sqlite3 process.** The `ipad` and `opad` blocks are
  compressed at startup. A token then costs its own blocks plus one outer block, about 1.5 ms.
- **Checks follow the spec.** Signature, `alg` = HS256, `exp`, a positive integer `sub` and a
  string `username`, each with the spec's 401 message.

## Why Bash at all

The `sqlite3` CLI can start processes (`.system`, `.output |cmd`), but it can't create, accept or
incrementally read a socket. Its only streaming input is SQL text on stdin, and `readfile()` and
`.import` read to EOF.

`/proc` doesn't help either:
- `/proc/net/*` is read-only;
- reopening a socket through `/proc/<pid>/fd/N` fails with `ENXIO`;
- Bash's `/dev/tcp` only connects.

The one thing that listens without a server binary is Bash's `accept` loadable. It binds, listens
(backlog 1), accepts one connection and closes the listener.

## Holding many connections without select()

Neither Bash nor the sqlite3 CLI has `select`, `poll` or `epoll`, so each open connection gets its
own Bash process. These processes are forked from a small main shell and do no forking themselves.

- **Shared sqlite3 pool.** Connections share a few sqlite3 processes (`POOL`, default 2) through
  FIFOs, instead of ~2 MB each for a sqlite3 per connection. The request goes in a per-connection
  file and the FIFO gets a single `.read` line. Bash line-buffers stdout, so a multi-line write
  would interleave with other connections' writes; one line is one atomic write.
- **Responses come back through a FIFO.** sqlite3 writes each response into the connection's own
  FIFO with `.once`, length first. Bash reads it with `read -N` and copies it to the socket.
- **One listener at a time.** Idle workers pass a one-byte token through a FIFO. The holder
  listens, hands the token on as soon as it has a connection, and asks the main shell for a
  replacement through another FIFO. When workers forked their own replacements, the process tree
  became as deep as the number of connections, and the kernel's per-process memory (anon_vma
  chains) grew with that depth.
- **Nothing on the request path forks.** No `$(...)`, no external commands; `mkfifo`, `rm`, `sleep`
  and `accept` are Bash loadables.

## Limits, measured

Measured in Docker on an Apple M5 (arm64, Linux 7.0), not the x86 box:
- server container: 1 CPU, 2 GB, no swap;
- load: the organizer's load model (mload: think time ×1, no gzip), 60 s ramp and 60 s hold.

| users | result | p95 | p99 | errors | server CPU |
|---|---|---|---|---|---|
| 500 | pass | 2.7 ms | 4.2 ms | 0.18 % | ~3 % |
| 2,000 | pass | 3.7 ms | 8.8 ms | 0.45 % | ~17 % |
| 2,500 | pass | 2.3 ms | 3.8 ms | 0.39 % | ~15 % |
| 3,000 | pass, but at the 2 GB limit | 3.0 ms | 5.2 ms | 0.58 % | ~22 % |

- **Memory is the ceiling, not CPU.** Each connection costs ~0.4–0.5 MB of private Bash memory
  (mostly heap, growing slowly with requests served) plus ~0.1 MB of kernel memory (page tables,
  slab, stack). At 2,500 users that is ~1.4 GB before the database's page cache. At 3,000 the
  container reached its 2 GB limit and the page cache was squeezed, so a 5-minute hold there would
  probably not survive. Expect about **2,500** on the box.
- **The errors are refused or reset connections during the ramp.** A connection that arrives in
  the microseconds between one worker closing the listener and the next opening it is refused,
  and the backlog is 1. Each user connects once, so these errors stay roughly fixed while the
  request count grows over a 5-minute hold.
- **Not supported:** chunked request bodies (Content-Length only) and `Connection: close`; a
  connection stays open until the client closes it.

## Files

| | |
|---|---|
| `server.sh` | Bash: accept, read requests, hand them to SQLite, copy responses back |
| `sql/app.sql` | the parser trigger, views for the post objects, the feed and the HTTP response |
| `sql/route/*.sql` | one file per route; `auth.sql` checks the JWT |
| `sql/gen-hmac.sql` | SQL that writes `sql/hmac.sql` (HMAC-SHA256) at build time |
| `install.sh`, `build.sh`, `start.sh` | apt packages; generate `hmac.sql`; run `server.sh` |
