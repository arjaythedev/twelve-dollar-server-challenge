# WebAssembly (WAT) inside the stock `sqlite3` shell

Direct HTTP/1.1 on `$HOST:$PORT` (no Nginx). The process that serves the API is SQLite's own command-line
shell. `start.sh` runs it on the database, and `start.sql` loads one small extension and calls one function
that never returns:

```sql
.load ./build/serve
SELECT serve('build/app.so');
```

That function starts WasmEdge, and the server, written by hand in WebAssembly's text format, runs inside
the `SELECT`, on the shell's own database connection.

```
start.sh      exec build/sqlite3 "$SQLITE_PATH" < start.sql
sqlite3       the stock shell, built from the SQLite 3.54.1 amalgamation (shell.c + sqlite3.c, unmodified)
 └ serve.so   src/serve.s: x86-64 assembly, the only native code of this entry
    └ libwasmedge   WasmEdge 0.18.1, used as a library
       └ app.so     src/app.wat, compiled ahead of time by `wasmedge compile`: HTTP, io_uring, JWT, SQL
```

## What is ours

| File | Language | What it does |
|---|---|---|
| `src/app.wat` | WAT, hand-written, ~1,250 lines | Everything: the io_uring event loop (it writes every submission entry and reads every completion itself), HTTP/1.1 parsing, routing, JSON reading, base64url, SHA-256 and HMAC for the HS256 check, and every SQL call |
| `src/serve.s` | x86-64 assembly, 158 lines without comments (136 instructions) | A SQLite extension. Its init registers `serve()`; `serve()` creates a WasmEdge VM, gives the module three imports and runs its `run` export |
| `start.sql` | SQL / shell commands | Pragmas for the shell's connection, `.load`, `SELECT serve(...)` |

There is no C, C++, Rust or Zig of ours, no generated C, no second process and no helper thread. SQLite is
not compiled to wasm: the WAT uses the shell's native SQLite.

The three imports (module `host`, each `(i64 x 7) -> i64`):

- `sys(nr, a1..a6)`: a Linux system call, straight to the kernel.
- `call(fn, a1..a6)`: calls a native function by its address.
- `membase()`: the native address of the module's linear memory, which the kernel and SQLite need for
  pointers into it.

## How the WAT reaches SQLite

SQLite hands every extension a pointer to `sqlite3_api_routines`, its table of public API functions. `serve()`
passes that pointer and its own `sqlite3_context` to `run`. The WAT copies the table into its memory with one
`process_vm_readv` on its own pid (a WAT cannot read native memory directly), then calls `sqlite3_prepare_v2`,
`sqlite3_bind_*`, `sqlite3_step`, `sqlite3_column_*` and `sqlite3_reset` through `call`, using the offsets of
those functions in the table (fixed for the pinned SQLite version). Pointers it passes in are `membase` plus
an offset. `sqlite3_context_db_handle` gives it the shell's connection, which `start.sql` opened on
`$SQLITE_PATH`.

Text that SQLite returns lives in native memory. The WAT copies it into its own memory with
`sqlite3_snprintf(n + 1, dst, "%s", text)`. To keep that to one copy per request, SQLite writes the JSON
of each post itself with `json_object` (its arguments are in the key order SPEC.md asks for), and the feed
is one row: the 20 objects joined by `group_concat`. The WAT writes the HTTP headers and copies that one
string after them.

## Design

- **One thread, io_uring, direct.** The rings live in wasm memory: with `IORING_SETUP_NO_MMAP` the kernel uses
  memory the process gives it, so the module can read and write the rings without any host code. Each ring
  is a single page (64 submission entries, 128 completions), so the memory never has to be physically
  contiguous across pages. A multishot accept takes new connections; each connection has a 4 KiB receive
  buffer, read with a single-shot recv.
- **Sending.** The responses to the complete requests in a buffer go out with one `sendto(MSG_DONTWAIT)`. If
  the socket takes only part of it, the rest is copied to a spill block and sent through io_uring, and reading
  resumes once it is out.
- **Writes.** Each POST is its own autocommit transaction on a WAL database with `synchronous=NORMAL`. The
  response is written only after `sqlite3_step` has returned `SQLITE_DONE`, so the row is committed when the
  201 goes out. `locking_mode=EXCLUSIVE`, since the shell's connection is the only one.
- **Configuration.** The WAT reads `JWT_SECRET`, `HOST` and `PORT` from `/proc/self/environ`; the shell opens
  `$SQLITE_PATH`. It raises its own soft `RLIMIT_NOFILE` to the hard limit (at most 65,536, the size of its
  per-connection tables).

## Rules

1. Any language: WAT, plus a page of assembly.
2. SQLite in-process: the server is the `sqlite3` process, on the file at `$SQLITE_PATH`. No other database or cache.
3. The schema is untouched. Only `SELECT` and `INSERT` statements.
4. `test/test.sh`: 42/42.
5. No caching: every request runs its SQL while it is served. Only prepared statements are kept.
6. WAL with `synchronous=NORMAL`; a 201 is sent after the commit.
7. Nothing depends on the load test.
9. Direct, on `$HOST:$PORT`.
10. Built from source on the box: `build.sh` compiles the stock `sqlite3` shell from the pinned amalgamation
    (sha256 checked), assembles `serve.s` and compiles `app.wat`. `install.sh` installs wabt and WasmEdge from
    official release tarballs, pinned by version and sha256, and `build-essential` from apt.
11. Nothing on the box is tuned. The file limit is raised inside the process, up to its own hard limit.

## Versions

- SQLite 3.54.1 (amalgamation: shell and library), built with gcc from Ubuntu 24.04, `-O2 -DSQLITE_THREADSAFE=0
  -DSQLITE_DEFAULT_MEMSTATUS=0` (compile options only; one thread uses the connection)
- WasmEdge 0.18.1 (AOT compiler and runtime library), wabt 1.0.39 (`wat2wasm`)
- GNU as / ld from Ubuntu 24.04 (binutils)

## Limits

- x86-64 Linux only (the assembly and the syscall numbers).
- A request larger than its 4 KiB buffer closes the connection. The test suite's largest request is about 1 KiB.
- Up to 65,536 open connections (the per-connection tables are indexed by file descriptor).
- The rings use `IORING_SETUP_NO_MMAP` and `IORING_SETUP_NO_SQARRAY` (Linux 6.5 and 6.6).
