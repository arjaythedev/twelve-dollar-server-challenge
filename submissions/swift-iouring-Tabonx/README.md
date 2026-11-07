# Swift, io_uring, and SQLite read straight off the B-tree

| | |
|---|---|
| Language | Swift 6.4.1 in Swift 6 language mode, no Foundation, stdlib linked statically |
| Framework | none. io_uring through liburing 2.15, with an epoll fallback for kernels that refuse it |
| SQLite | 3.54.0 amalgamation, compiled into the binary with profile-guided optimization |
| Nginx or direct | direct, on `0.0.0.0:80` |

One thread does everything. It parses HTTP, checks the JWT with its own SHA-256, walks SQLite's
B-tree, and writes the JSON. A second thread only copies SQLite's write-ahead log back into the
database file, at nice 19.

## Swift without the parts people usually write

Most of the Swift here looks more like C than like an iOS app, and that was the point of the
exercise: to see how close Swift gets when you stop reaching for its conveniences.

- No `String` on the request path. A request is a set of `ByteView`s, a pointer and a length,
  pointing into the buffer the kernel received into. Header names, routes and JSON keys are
  `StaticString`s compared byte by byte.
- Responses go into an `OutBuffer`, a `~Copyable` struct that owns its memory. The compiler
  refuses to copy it, so there is always exactly one owner and no reference counting.
- No `Codable`. The JSON reader walks the bytes once, validates the whole document, and calls a
  non-escaping closure for each top-level member. Strings without escapes come back as views into
  the request, never copied.
- SHA-256 and HMAC are written in Swift, about 140 lines.
- C appears only where Swift can't reach. liburing is mostly macros and inline functions, and the
  B-tree code needs SQLite's internal headers, so those live in small C targets that Swift imports
  through module maps.
- Built with `-Ounchecked` and `-enforce-exclusivity=unchecked`, arithmetic written with the
  wrapping operators (`&+`, `&-`), so the hot loops carry no overflow or bounds checks.

## What a framework costs

Before this entry I put the same request handling behind five different Swift servers and
measured CPU per request in a 1-CPU Linux container on an M1. The numbers are local and won't
match the leaderboard, but the gaps between them are what I found interesting:

| Server | µs per request |
|---|---:|
| Vapor 4 | 59 |
| Hummingbird 2 | 40 |
| SwiftNIO, handler called synchronously on the event loop | 16.5 |
| plain epoll, no NIO | 11.3 |
| plain epoll, built as Embedded Swift (no runtime) | 12.0 |

The biggest single step is the hop from NIO's event loop into async/await and back, which cost
more than the whole request did. Dropping the Swift runtime with Embedded Swift changed nothing
measurable, so the runtime isn't where the time goes. This entry is the plain loop, moved to
io_uring.

## One pass, one transaction

The loop waits up to a millisecond for completions, then handles everything that arrived as one
pass. Every request in the pass runs inside the same SQLite transaction, the pass commits once,
and only then do its responses go to the kernel. A few dozen requests share each commit, and a
read never reports a row that might still roll back.

The first version of this entry ran eight rings on eight threads that took turns on SQLite
behind a mutex. On one CPU that buys nothing. The threads never run at once, they just wake each
other up, and every one of them commits its own small batch.

## Reading SQLite without SQL

A feed is 20 posts with their authors and like counts. As SQL that is a join with a correlated
subquery, which SQLite compiles to bytecode and runs in its virtual machine. The data access
underneath is small: walk an index for 20 entries, look up 20 posts by id, look up 20 users by
id, and count entries in one range of another index.

`Sources/CSQLite/db.c` does exactly those steps. It `#include`s the unmodified amalgamation so it
can open SQLite's own B-tree cursors, and Swift decodes the record bytes the cursors point at.
Nothing is cached between requests, every row comes out of SQLite while the request is served,
and writes still go through ordinary SQL.

Things that turned out to matter:

- The 20 posts are looked up in ascending id order, the reverse of the feed. SQLite has a fast
  path for a cursor asked for the id right after the one it is on: it steps to the next cell
  instead of searching from the root. New posts get consecutive ids, so most lookups are one step.
- Like counts come from a single walk over `likes_post_id_idx`, counting cells a page at a time
  without decoding each one.
- The plan trusts nothing about the data. If an index entry has no row, or a post has no author
  (where the SQL join would drop it), the plain SQL query answers instead. At startup the server
  checks the column order of every table and index it decodes and refuses a different schema.

## A checkpoint that never finishes

In WAL mode SQLite appends each commit to a log and later copies the log back into the database.
The log can only start over once a copy has caught up with all of it. Under a steady stream of
likes and posts that never happens on its own, because new frames land while the copy runs, and a
longer log makes every page read slower.

A background thread with its own connection copies the bulk of the log while requests carry on.
When it finishes, writes wait for at most 250 ms while it copies the last few frames, so the log
can start over. A waiting write isn't refused. Its bytes stay in the connection's buffer and the
request runs when the wait ends. Reads never wait.

## Where the connections live

io_uring can accept a socket straight into a ring's own file table, as a direct descriptor that
never appears in the process's descriptor table. Each ring's table holds as many sockets as
`RLIMIT_NOFILE` allows, and the server adds another ring when one gets close to full. It moves on
1,024 slots early, because an accept that finds no free slot has already taken the connection off
the listen queue and the kernel drops it. Eight listening sockets share the port through
`SO_REUSEPORT`, so a burst of new connections spreads over eight listen queues.

A connection that is only open costs the server a 64-byte slot. Receives go into a ring of 1,024
shared 2 KiB buffers that the kernel picks from and the server hands back after each pass, and
responses are written into a shared arena and sent from there without a copy. That matters more
than CPU, because the kernel already needs about 3.4 KB for every TCP socket, and the machine has
2 GB.

I found out how much it matters by getting it wrong. A request that waited out a checkpoint kept a
4 KB input buffer afterwards, for as long as its connection stayed open. Short local tests never
showed it. On a real 1 vCPU, 2 GB machine with a few hundred thousand connections, it leaked 2.4 MB
a second and the kernel killed the server three minutes in. The buffer is now freed the moment it
is empty.

## Smaller things

- JSON output is scanned eight bytes at a time for characters that need escaping, and copied in
  one piece when there are none.
- Every token's signature is checked on every request. The HMAC key's inner and outer blocks are
  hashed once at startup, so a check hashes only the token plus one more block.
- SQLite's PGO profile is trained during the build on a synthetic database made from
  `schema.sql`, running the same reads and writes the server does.
- `body` is trimmed with the same whitespace set as JavaScript's `trim()` and counted in code
  points, which is what the schema's `CHECK (length(body) <= 500)` counts.

## Where things live

| | |
|---|---|
| `Sources/Server/Ring.swift` | the io_uring loop: rings, accepts, receives, sends, the pass |
| `Sources/Server/Database.swift` | record decoding, the feed and post plans |
| `Sources/Server/Service.swift`, `HTTP.swift` | routing, the five endpoints, HTTP framing |
| `Sources/Server/JSON.swift`, `JWT.swift`, `SHA256.swift` | parsing, escaping, HS256 |
| `Sources/Server/Bytes.swift` | `ByteView` and `OutBuffer` |
| `Sources/Server/EpollServer.swift` | the fallback loop for kernels without io_uring |
| `Sources/CSQLite/db.c` | the amalgamation, B-tree cursors, transactions, the checkpoint thread |
| `Sources/CUring`, `Sources/CShim` | liburing helpers and socket setup |
| `pgo/train.c` | the training workload for the SQLite profile |
