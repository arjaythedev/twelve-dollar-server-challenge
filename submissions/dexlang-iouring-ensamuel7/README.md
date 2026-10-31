# DexLang (generated C, io_uring)

| | |
|---|---|
| **Language** | [DexLang](https://github.com/ensamuel7/dex) `v0.9.26`: statically typed, compiles to C, which `cc` builds into a native binary |
| **Framework** | None. DexLang's own `http` and `db` standard library modules |
| **Server** | The `http` module's io_uring backend (`http.uring()`), with handlers run on the ring's thread |
| **SQL** | Built with [dexqu](https://github.com/ensamuel7/dex-modules/tree/main/dexqu) `v0.1.0` at startup, prepared once |
| **SQLite driver** | SQLite 3.54.1, the official amalgamation compiled into the binary by the `db` module (`DEX_SQLITE`), pinned and sha256-checked in `build.sh` |
| **Build toolchain** | Go 1.27.2 to build the compiler, then `cc` (gcc 13) on the C it emits |
| **Nginx or direct** | **Direct**, on `HOST:PORT` (`0.0.0.0:80` on the benchmark box) |

DexLang compiles to C. What runs is one native binary with SQLite compiled in:
no interpreter, no garbage collector, no framework. The program is about 360
lines of DexLang in `server.dx` and `queries.dx`, and everything below it is the
language's standard library, unmodified for this challenge.

## How it serves

**io_uring, opted into with one call.** `http.uring()` switches the `http`
module from its epoll loop to an io_uring one. Accepts, receives, sends,
shutdowns and closes are queued on one ring and submitted together, so each pass
of the loop is one system call however many connections it served, with up to 64
accepts in flight. The ring is set up with `SINGLE_ISSUER | DEFER_TASKRUN` and
uses raw `io_uring_setup`/`io_uring_enter`, so there is no liburing dependency.
If a machine refuses io_uring (an old kernel, or a container that blocks it), the
server says so and serves the same routes with epoll.

**Several rings of direct descriptors, on one thread.** Accepted sockets go into
a ring's registered file table, which doesn't use the process's descriptors. The
kernel caps one table at the open-file limit, so `http.limits(0, 75, 200000)`
makes the runtime open three rings on the same thread, each with its own 65,535
slots, plus ordinary descriptors once those are full: about 262,000 open
keep-alive connections under `LimitNOFILE=65535` before anything has to
reconnect (it was 131,000 with one ring). The thread sleeps in epoll on the
rings' eventfds when none has work. Past fifteen sixteenths of the limit,
responses carry `Connection: close` so idle users reconnect for their next
request; at the limit, new connections wait in the listen queue instead of
being refused.

**No read buffer per idle connection.** Receives take a buffer from a ring the
kernel fills (`IORING_REGISTER_PBUF_RING`), and the bytes are copied out at
once into a pooled per-request buffer that goes back when the response is
sent. A keep-alive user thinking between requests holds no read buffer, so a
quarter of a million of them do not need half a gigabyte of buffers.

**Handlers run on the ring's thread.** There is no handoff to a pool, no lock
and no wakeup per request. On one vCPU with handlers that only touch an SQLite
file, that is the cheapest shape. Each pass of the loop runs the handlers for
every request that arrived, then the batch hook below, then queues all the
responses.

**The same protections as the default server.** 32 KB header cap (431), 8 MB
body cap (413), and a 75 s idle timeout that shuts a stalled connection down.
Both backends were run under AddressSanitizer and UndefinedBehaviorSanitizer
against slow, oversized, aborted and reset clients, with no findings.

## The database

- **The six statements are built once with dexqu** (`queries.dx`) and prepared
  at startup. A request only binds and steps. The feed walks
  `posts_created_at_id_idx` for 20 rows with no sort, and the like count
  resolves through `likes_post_id_idx` as a covering index.
- **Group commit (`http.afterBatch`).** A write handler opens a transaction if
  the batch has none (`beginWrite`); `commitBatch` commits it once per pass of
  the loop, and the runtime holds every response in that pass until it
  returns. So a `201` still goes out only after its row is committed (rule 6),
  but under load one commit and one WAL write cover all the likes and posts of
  a pass instead of one each. If the commit fails, the runtime answers every
  request in the batch `500 {"error":"internal server error"}` rather than
  what its handler said; nothing is acknowledged that is not durable.
- **Every statement is reset as soon as it's read.** A statement left on a row
  keeps the connection's read transaction open, so an `INSERT` on the same
  connection wouldn't commit until the next reset. Resetting first means a 201
  goes out after its row is committed (rule 6), and lets the WAL checkpoint.
- **One connection, `locking_mode=EXCLUSIVE`, opened with
  `db.openSingleThreaded`.** Only the ring's thread touches SQLite, so it takes
  the file lock once instead of a lock round-trip per request, and skips the
  mutex SQLite otherwise takes on every call. Checkpoints use SQLite's own autocheckpoint, with
  `journal_size_limit` keeping the WAL file at 64 MB at most.
- **`ON CONFLICT DO NOTHING ... RETURNING`** answers "liked now" and "already
  liked" in one statement.
- **SQLite is compiled in, not linked.** `build.sh` downloads the official
  3.54.1 amalgamation, checks its sha256, and dex compiles it with the program
  under the same `-O3 -flto`, so calls into SQLite can be inlined. It is built
  with the same features and limits as Ubuntu's `libsqlite3` (FTS, R-tree,
  `SECURE_DELETE` and so on) and stays thread-safe. Measured on this workload,
  about 15% fewer instructions per request than the shared library.

```
locking_mode       = EXCLUSIVE
journal_mode       = WAL
synchronous        = NORMAL      rule 6: a commit reaches the WAL before the response
busy_timeout       = 5000
cache_size         = -65536      64 MB of pages
mmap_size          = 268435456   256 MB, covering the 238 MB seed file
temp_store         = MEMORY
journal_size_limit = 67108864
```

No response or query result is cached. Every request reads SQLite.

## Responses

Every response body is `json.encode` of a struct or an object literal, so the
wire shapes are written down once and no JSON is assembled by hand:

```dex
let p: Post = Post {}
db.fill(p, stOne)
db.reset(stOne)
return http.response(200, json.encode(PostEnvelope { post: p }), "application/json")
```

`db.fill` reads a row into the struct by column position, and field order is key
order, which is the order the spec fixes. The compiler generates one encoder per
struct: keys are literals, numbers go through an integer writer, and a string
with nothing to escape is copied in one `memcpy`. A 20-post feed page encodes in
about 1.3 µs.

## Auth

HS256 is verified on every request: the MAC is recomputed in base64url and
compared to the signature segment, so there's no decode on the hot path. `alg`
is checked explicitly (a valid HMAC over a header saying `HS512` is the
algorithm-confusion bug), and `exp` is required. A token without it is a 401.

## What the language gained from this challenge

All of it went into DexLang's standard library and compiler, not into this
folder:

- `http.uring()`: the io_uring backend with direct descriptors described above.
- `http.inline()`: handlers on the event-loop thread, for the epoll backend.
- Connection shedding near the open-file limit, `SOMAXCONN` listen queue,
  `accept4` with `SOCK_NONBLOCK | SOCK_CLOEXEC`.
- A thread-safety fix: an `http` server with no `mutex` or `spawn` of its own
  was built single-threaded although its handlers ran on a pool, which corrupted
  the allocator under concurrent requests. Found with ThreadSanitizer.
- Two `json.encode` code-generation fixes (arrays of enums in a struct, and a
  struct literal whose fields call functions in a program that throws).
- Per-request path and query storage instead of 4 KB fixed buffers per
  connection.
- A generated `json.encode` per struct, about 9x faster than the
  field-descriptor encoder it replaces and as fast as writing the bytes by hand,
  with byte-identical output; and a fix for struct literals passed straight to
  it, which leaked the values they created.
- `db.openSingleThreaded`, a connection without SQLite's per-call mutex for
  code that only ever uses it from one thread.
- `DEX_SQLITE`, which compiles a pinned SQLite amalgamation into the binary
  instead of linking the system library.
- `db.reset` clears parameters one at a time instead of with
  `sqlite3_clear_bindings`, which in SQLite 3.54 forces some statements to be
  re-parsed on every use.
- A heap overflow in response building when a handler passed a content type
  longer than about 400 bytes, found while replacing its `snprintf`.
- `req.headers` built the first time a handler uses it, not for every request.
  The map owns a copy of the header lines, so one kept past its request is
  still valid, and it reads the same as parsing up front: the HTTP transcript
  tests' new cases were recorded with the previous runtime and reproduce byte
  for byte. Here only the like and post handlers read a header, so the feed
  and single-post requests no longer build the strings.
- A data race between the thread pool and the idle sweep over a connection's
  state, found with ThreadSanitizer, and a macOS kqueue bug (filter deletes
  dropped after a failed one) that left connections reported twice per wait
  and freed under the event loop. Neither path runs on the benchmark box.
- Several io_uring rings on one thread when `http.limits` asks for more
  connections than one ring's descriptor table holds, receives through a
  kernel buffer ring instead of a buffer per connection, and an idle sweep
  that reads compact per-slot timestamps rather than every connection.
- `http.afterBatch`, the group-commit hook above. Under io_uring a batch is a
  pass of the loop; on epoll and the thread pool it is one request, so code
  written for it is correct on every backend.

## Files

```
server.dx    routes, handlers, auth, response building
queries.dx   the six SQL statements, built with dexqu
dex.mod      module file: dex 0.9.26, dexqu v0.1.0 (pinned in dex.sum)
install.sh   root, once: build-essential, git, pkg-config, libsqlite3-dev,
             libssl-dev, libcurl, and Go 1.27.2 (sha256-checked)
build.sh     builds the dex compiler from the pinned v0.9.26 tag through the Go
             module proxy (checksum-verified), fetches dexqu and the SQLite
             3.54.1 amalgamation (sha256-checked), compiles server.dx
start.sh     raises the soft open-file limit to the hard one and runs the server
```
