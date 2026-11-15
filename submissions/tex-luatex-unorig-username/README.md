# tex-luatex: the web server is TeX

`start.sh` runs an IniTeX job, `luatex --ini server.tex`, that never reaches `\end`. TeX's own main
control loop is the event loop, and every JSON body the API sends is the expansion of a TeX macro.
Nothing is typeset; the DVI file stays empty forever.

| | |
|---|---|
| Language | TeX (the loop and the JSON envelopes) and Lua 5.3 (HTTP, JWT, SQL), both inside LuaTeX |
| Runtime | LuaTeX 1.17.0 from Ubuntu's `texlive-binaries 2023.20230311.66589-9build3` (plain Lua 5.3: Ubuntu builds no LuaJITTeX for amd64) |
| Network | LuaTeX's built-in luasocket 3.0-rc1 (`select`) |
| SQLite | [lsqlite3](http://lua.sqlite.org/) 0.9.6 (MIT), compiled by `build.sh` into a Lua C module that LuaTeX loads in-process with `package.loadlib`; system SQLite 3.45.1 (`libsqlite3-dev`) |
| JWT | HMAC-SHA256 (RFC 2104) over LuaTeX's built-in `sha2.digest256`, base64 from luasocket's `mime` |
| JSON | Posts are built by SQLite's `json_object`; requests are parsed by a strict RFC 8259 parser in `server.lua` |
| Front | **Behind Nginx** (`bench/nginx.conf`), listening on `127.0.0.1:3000` |

## Why Nginx

luasocket waits with `select`, which cannot watch a descriptor above 1023, so 15,000 direct keep-alive
connections are out of reach. Nginx keeps a few dozen upstream connections; the server stops accepting at
900 and resumes when one closes, so a spike waits in the kernel backlog.

## How it works

- **The loop is a TeX macro.** `server.tex` ends with

  ```tex
  \def\serve^^A\luafunction1 \serve^^B
  \serve
  ```

  (`^^A` and `^^B` are the grouping characters, so `{ } [ ] " :` are plain characters and the macros can
  spell JSON as is). `\serve` is tail-recursive, so TeX's input stack stays flat. `\luafunction1` waits in
  `select` (up to 1 s), accepts, reads, and answers every complete request in the buffers: pipelined and
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

## Numbers (local, rough)

Not the benchmark box. Server and Nginx (1 worker) in one 1-CPU, 2 GB, no-swap, `nofile=65535` x86_64
container emulated on an ARM laptop VM that was running other benchmarks at the same time; the database
copy was on tmpfs. k6 `bench/load.js` from separate containers, 60 s ramp, **1 minute** hold.

| VUs | req/s | p95 | p99 | failed | CPU (container) | luatex RSS |
|---|---|---|---|---|---|---|
| 60,000 | 4,485 | 1.3 ms | 6.3 ms | 0% | 86-94% | 74 MB |
| 80,000 | 5,970 | 23 ms | 36 ms | 0% | 100% | 78 MB |
| 90,000 | 6,440 | 284 ms | 1.7 s | 0.03% | 100% | 95 MB |
| 100,000 | 4,330 | 13 s | 55 s | 14% | 100% | 110 MB |

So about 80,000 here; on this laptop setup other entries have scored 1.3 to 2.5 times higher than on the
droplet. With `wrk` (64 connections, 100 feed : 100 post : 15 like : 2 create, no think time) it serves
about 17,000-20,000 req/s through Nginx.

`test/run.sh`: 42 passed, 0 failed (fresh ubuntu:24.04 amd64 after `install.sh`).

## Known limits

- Request bodies need `Content-Length`; chunked uploads and `Expect: 100-continue` are not handled, GET
  bodies are not skipped, and `Connection: close` is left to the client. A request over 1 MB drops the connection.
- A query string is ignored; a trailing slash makes a path unknown (404).
- `install.sh` downloads lsqlite3 from lua.sqlite.org (SHA-256 checked); if that site is down, the install fails.
