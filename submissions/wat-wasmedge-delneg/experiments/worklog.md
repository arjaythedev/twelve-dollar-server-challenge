# WAT entry worklog (opt-box: i5-11400F VM, 4 vCPU, x86_64; NOT the grader)

Harness: `ub.sh` starts the server pinned to CPU 0 on a fresh copy of the seed (on disk, `~/watlab/wb`),
then `ul` (own C load generator, `bench/load.js` user loop with think times x SCALE, keep-alive,
reconnects on `Connection: close`) on CPUs 1-3. us/req = server process CPU (utime+stime) / requests
in the measure window. Loopback: part of the server's kernel TCP work is charged to the client cores.

## Setup (1 Nov)
- opt-box `~/watlab`: wasi-sdk 34 (clang 23.1.0), wabt 1.0.39, binaryen 125, WasmEdge 0.18.1,
  SQLite 3.54.1 amalgamation (sha256 0a9c8e34… = the ObjC entry's pin). Seed copied from `~/tdsc/repo/seed`.
- Reference `wserve.c` (C, poll loop) native: 42/42; WasmEdge AOT: 42/42.
- WasmEdge 0.18.1 source read (`lib/host/wasi/inode-linux.cpp`): `poll_oneoff` keeps one epoll fd but
  does an `epoll_ctl` ADD/MOD for every subscription on every call, DELs the ones not passed again,
  and an `ioctl(FIONREAD)` per ready fd. So a call costs O(subscriptions) syscalls. WASI fds are random
  32-bit numbers (an `unordered_map`), so the program cannot index by fd; poll userdata carries the slot.
- WasmEdge's listener needs `sock_setsockopt(SO_REUSEADDR)` or a restart fails to bind while old
  connections sit in TIME_WAIT ("wasmedge socket setup failed").

## E1: keep-alive vs a new connection per request (Connection: close), C reference
4000 users, scale 0.1 (~4.2k req/s), 15 s, 3 interleaved reps.
| variant | us/req (3 reps) | p99 ms |
|---|---|---|
| native, keep-alive (poll over ~4000 fds) | 189.3, 188.4, 189.0 | 34-37 |
| native, Connection: close | 17.48, 17.49, … | 34-38 |
| WasmEdge AOT, keep-alive (epoll_ctl per fd per call) | 134.5, 137.7, … | 38-42 |
| WasmEdge AOT, Connection: close | 20.19, 19.51, … | 33-36 |
Finding: with every user holding an idle keep-alive connection, the per-call O(n) poll dominates
(7-10x). Closing after every response keeps the poll set tiny. load.js think times are >= 3 s, so
keeping a connection for less than 3 s never gets reused anyway, and keeping it longer at 280k users
would need ~80k+ fds (LimitNOFILE is 65,535). Decision: answer with `Connection: close`.
p99 ~35 ms in every variant: probably the inline WAL checkpoint (to check).

## Linking test (1 Nov)
`wat2wasm main.wat` + SQLite reactor module (wasi-sdk clang, `-mexec-model=reactor`, `--global-base=16 MiB`)
joined by binaryen `wasm-merge sq.wasm sq main.wasm main`: the WAT module imports the SQLite module's
memory and functions; runs under WasmEdge interpreter and AOT. wasm-ld default is stack-first
(stack [0, 64 KiB)); with `-z stack-size=1 MiB` and `--global-base=16 MiB` the WAT owns [1 MiB, 16 MiB).

## SQLite survey (user request, 1 Nov)
How the top entries use SQLite (grep of their sources):
- All: WAL + `synchronous=NORMAL`, one connection, persistent prepared statements, group commit per batch,
  `temp_store=MEMORY`, `unix-excl` VFS (WAL index on the heap, no shm), mostly `mmap_size=0`
  (ObjC measured a 32 MB page cache ~3% faster than mmap), cache_size -512 KiB … -64 MiB.
- Checkpoints off the event loop: `wal_autocheckpoint=0` and PASSIVE checkpoints from a second
  connection on a thread (Lean, Mojo, pavlyhalim, public, shivanshthapliyal, V) or a forked child (ObjC),
  with a write gate around 8-16k frames. c-iouring-public's inline "bounded" mode held 17.5k vs 15.5k req/s
  depending on pass size: inline checkpoints stall the loop.
- Compile flags (Mojo/ObjC/public/pavlyhalim share most): THREADSAFE=0/2, DEFAULT_MEMSTATUS=0,
  DEFAULT_WAL_SYNCHRONOUS=1, DQS=0, LIKE_DOESNT_MATCH_BLOBS, MAX_EXPR_DEPTH=0, OMIT_DEPRECATED,
  OMIT_SHARED_CACHE, OMIT_PROGRESS_CALLBACK, OMIT_LOAD_EXTENSION, USE_ALLOCA, OMIT_DECLTYPE,
  (pavlyhalim also OMIT_AUTOINIT/JSON/UTF16/TRACE, TEMP_STORE=3, own PCACHE2 plugin).
- Fastest reads: SQLite B-tree cursor calls from a TU that includes the amalgamation (ObjC, Mojo,
  pavlyhalim, public, V).

Existing wasm SQLite builds, checked for WASI use:
- SQLite official `ext/wasm`, sql.js, wa-sqlite: Emscripten, browser VFS (OPFS/IndexedDB). Not WASI.
- ncruces/go-sqlite3 (build now in ncruces/go-sqlite3-wasm `build/build.sh`): freestanding `--target=wasm32`,
  own libc, `SQLITE_OS_OTHER` with the VFS in Go on the host, `wasm-opt -O4 --gufa-optimizing …`, then
  wasm2go. Not WASI (its VFS is host code). Reusable: its option list (`sqlite_opt.h`) and the wasm-opt pass.
- VMware Wasm Labs (webassembly-language-runtimes) libsqlite: their patches were upstreamed;
  since 3.41.0 stock SQLite builds for WASI (`SQLITE_WASI`: no fchmod/mremap, default VFS
  `unix-dotfile`, no shm). Their article says WAL is off in their build; with
  `locking_mode=EXCLUSIVE` WAL works with the index on the heap (the reference 42/42 runs that way).
Decision: build the stock pinned amalgamation with wasi-sdk (that is the WASI-ready path), borrow
flags from the top entries / ncruces, and A/B: flag set, wasm-opt on the SQLite module, page cache,
VFS (`unix-none` avoids a stale `.lock` dir after a crash), checkpoint policy.
Open: checkpoints. No fork/threads in plain WASI; WasmEdge wasi-threads support to check.

## E2: the WAT server, first build (1 Nov) — 42/42
Built by `experiments/dev-build.sh` on opt-box: SQLite 3.54.1 + `src/sqlite_glue.c` (wasi-sdk 34, `-O2`,
reactor, 1 MiB stack, data from 16 MiB) and `src/app.wat` (wat2wasm 1.0.39), joined by `wasm-merge`
(binaryen 125), `wasmedge compile --optimize=3` → `app.so` (3.7 MB), run by `src/host.c` (WasmEdge 0.18.1
C API, `WasmEdge_ConfigureSetRunMode(AOT)` is required or the .so is rejected).
Design: SPEC wants HTTP/1.1 keep-alive, so instead of E1's `Connection: close` the host gives the WAT
8 raw Linux calls ("sys": listen, epoll_create/ctl/wait, accept4, recv, send, close); level-triggered
epoll, per-fd state in a table indexed by the real fd. WASI is kept only for SQLite's file I/O, env and
clock. SQLite: one connection, `unix-none` VFS, locking_mode=EXCLUSIVE, WAL, synchronous=NORMAL,
64 MiB cache, persistent prepared statements, default autocheckpoint (inline).
Result: `t.sh e2 out/host out/app.so` → passed=42 failed=0 on the first full run.
