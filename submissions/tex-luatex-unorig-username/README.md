# tex-luatex: the web server is TeX

`start.sh` runs an IniTeX job, `luatex --ini server.tex`, that never reaches `\end`. TeX's own main
control loop is the event loop, and every JSON body the API sends is the expansion of a TeX macro.
Nothing is typeset; the DVI file stays empty forever.

| | |
|---|---|
| Language | TeX (the loop and the JSON envelopes) and Lua 5.3 (HTTP, JWT, SQL), both inside LuaTeX |
| Runtime | LuaTeX 1.17.0 from Ubuntu's `texlive-binaries 2023.20230311.66589-9build3` (plain Lua 5.3: Ubuntu builds no LuaJITTeX for amd64) |
| Network | `evio.c`, a 100-line Lua C module with epoll and raw socket calls, built by `build.sh`; luasocket only for `mime` base64 |
| SQLite | [lsqlite3](http://lua.sqlite.org/) 0.9.6 (MIT), compiled by `build.sh` into a Lua C module that LuaTeX loads in-process with `package.loadlib`; system SQLite 3.45.1 (`libsqlite3-dev`) |
| JWT | HMAC-SHA256 (RFC 2104) over LuaTeX's built-in `sha2.digest256`, base64 from luasocket's `mime` |
| JSON | Posts are built by SQLite's `json_object`; requests are parsed by a strict RFC 8259 parser in `server.lua` |
| Front | **Direct**, listening on `HOST:PORT` (`0.0.0.0:80`), no Nginx |

## Why direct, and what is in C

The first version sat behind Nginx and held 24,000 users on the benchmark box (62% CPU), failing just above
that on POSTs that got no response. Every entry behind Nginx that got there stopped at 24,000 with the same
errors, which points at Nginx's 16,384 `worker_connections` for 24,000 keep-alive users rather than at the app.

Serving directly means one keep-alive connection per user, and luasocket waits with `select`, which cannot
watch a descriptor above 1023. So `evio.c` replaces it: `listen`, `accept4`, `recv`, `send`, `close` and an
epoll set (level-triggered), nothing else. It parses nothing and decides nothing; the HTTP parser, JWT,
SQL, group commit and the loop itself stay where they were, in Lua and TeX inside the one LuaTeX process.
Every connection stays open until its client closes it.

## How it works

- **The loop is a TeX macro.** `server.tex` ends with

  ```tex
  \def\serve^^A\luafunction1 \serve^^B
  \serve
  ```

  (`^^A` and `^^B` are the grouping characters, so `{ } [ ] " :` are plain characters and the macros can
  spell JSON as is). `\serve` is tail-recursive, so TeX's input stack stays flat. `\luafunction1` waits in
  `epoll_wait` (up to 1 s), accepts, reads, and answers every complete request in the buffers: pipelined and
  split requests both work.
- **TeX writes the bodies.** Each answer is queued as TeX code, e.g. `\xdef\out^^A\liked^^Afalse^^B^^A42^^B^^B\luafunction2`,
  and at the end of the turn all of it goes to TeX in one `tex.sprint`. TeX expands each macro into `\out`
  and calls `\luafunction2`, which takes the body (`token.get_macro`) and adds the HTTP head. The macros are
  in `server.tex`: `\feed`, `\post`, `\liked`, `\health`, `\degraded` and `\oops` for errors. Post objects come from
  SQLite's `json_object` and are put in at an `@` in the envelope, so post bodies (user text) never pass
  through TeX's tokenizer: no TeX injection, and a feed would cost about 60 us more if they did.
  An expansion costs about 2 us. No user text reaches TeX at all: only fixed messages, integers and ids.
- **Every request reads SQLite**, one statement per read, prepared once. Nothing is cached between
  requests, and every token gets a fresh HMAC (two `sha2.digest256` calls over pads built once from
  `JWT_SECRET`), then the `alg`, `exp`, `nbf`, `sub` and `username` checks.
- **Group commit.** The first write of a turn opens `BEGIN IMMEDIATE`. `\luafunction3`, which TeX calls
  after the turn's bodies, commits and only then sends that turn's responses, one `send` per connection.
  A 201 never leaves before its `COMMIT` (WAL, `synchronous=NORMAL`). If a commit fails, the connections of
  that turn are closed without an answer. A partial `send` keeps the rest for when the socket is writable.
- **Why `--shell-escape`:** in TeX mode LuaTeX allows `package.loadlib` (and enables luasocket) only with it.
  Nothing here runs a shell command. `texmf.cnf` here is three lines, because the binaries package ships none
  and kpathsea needs one to find `server.tex`.
- `create` uses `INSERT ... RETURNING json_object(...)`, with the token's username bound as `?3`; a like is
  one `INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`, and only when it changes
  nothing does a second query tell "already liked" from "no such post".

## Numbers (local)

Not the benchmark box. The server runs in an x86_64 container emulated on an ARM VM, on one core capped at
half of it (1 ms of every 2 ms), `nofile=65535`, database on tmpfs. With that cap the merged version behind
Nginx uses 67% CPU at 24,000 users (rig: 62%). k6 `bench/load.js` from separate containers, 60 s ramp, 2 minute
hold unless noted.

| Users | req/s | p95 | p99 | failed | CPU (of the cap) | luatex RSS |
|---|---|---|---|---|---|---|
| 24,000, merged, behind Nginx | 1,996 | 1.3 ms | 7.3 ms | 0% | 67% | |
| 40,000 | 3,328 | 22 ms | 82 ms | 0% | 76% | 93 MB |
| 50,000 | 4,155 | 54 ms | 143 ms | 0% | 84% | 104 MB |
| 60,000 | 4,974 | 228 ms | 329 ms | 0% | 100% | 120 MB |
| 65,000 (2 runs) | 5,390 | 221 ms | 310-314 ms | 0% | 100% | 124-127 MB |
| **65,000, 5 minute hold** | **6,041** | **258 ms** | **330 ms** | **0%** | 99% | 138 MB |
| 70,000 (2 runs) | 5,575 | 201-231 ms | 305-334 ms | 0.95% | 100% | 128 MB |

At 70,000 the process is at its 65,535 descriptors: about 4,500 users are never accepted and their requests
fail, so 65,000 is the ceiling here, set by `LimitNOFILE` rather than by CPU. About 75 us of CPU per request.

`test/run.sh`: 42 passed, 0 failed (fresh ubuntu:24.04 amd64 after `install.sh`).

## Known limits

- Request bodies need `Content-Length`; chunked uploads and `Expect: 100-continue` are not handled, GET
  bodies are not skipped, and `Connection: close` is left to the client. A request over 1 MB drops the connection.
- One descriptor per connection and no idle timeout: past `LimitNOFILE` (65,535) new connections wait in the
  kernel backlog, and the listener stays readable, so the loop spins until a client leaves.
- A query string is ignored; a trailing slash makes a path unknown (404).
- `install.sh` downloads lsqlite3 from lua.sqlite.org (SHA-256 checked); if that site is down, the install fails.
