# C + epoll + SQLite

| | |
|---|---|
| Language | C (GCC 13.3, Ubuntu 24.04's `build-essential`), `-O2 -march=x86-64-v2 -flto`, profile-guided |
| Framework | none: one `epoll` event loop, hand-written HTTP/1.1, JSON and JWT code |
| SQLite | 3.54.1, the official amalgamation (SHA-256 pinned in `build.sh`), compiled into the binary |
| Other dependencies | none (SHA-256/HMAC, base64url and JSON are in `server.c`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |

## Running it

```bash
sudo bash install.sh   # apt: build-essential, curl, unzip
bash build.sh          # downloads and verifies SQLite, builds bin/server (profile-guided)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- **One process, one thread, one `epoll` loop.** The box has one vCPU, so threads would only add
  context switches. Every query is an indexed lookup that takes microseconds, so SQLite is called
  inline on the loop.
- **A transaction per loop iteration (group commit).** The first request in an `epoll_wait` batch that
  touches the database opens a deferred `BEGIN`. Every read and write in the batch runs inside it, and
  the batch ends with one `COMMIT`. Responses are written into an arena while the batch runs and are
  only sent after that `COMMIT` returns, so a `201` always means the row is committed (rule 6), and a
  read never reports data that could still be rolled back. If a commit fails, nothing from that batch
  is sent: those connections are closed instead. Reads still see every write committed before them
  (the like count is "as of this request").
- **Checkpoints happen off the request path.** In WAL mode, SQLite copies the log back into the
  database every 1,000 pages and fsyncs it, inline in whichever `COMMIT` crosses the line. That stall
  hit every client in the batch and set the p99. Here a small helper process (the same binary, started
  with `--ckpt`) runs `PASSIVE` checkpoints in a loop. The event loop never waits on the disk, and the
  WAL stays a few MB under normal load. If the helper can't keep up (the CPU is pegged for minutes),
  the server's own `wal_autocheckpoint=65536` still caps the WAL at about 256 MB. Durability is
  unchanged: WAL with `synchronous=NORMAL`, and a response waits for its `COMMIT`.
- **Profile-guided build.** `build.sh` first builds an instrumented binary with `train.c` compiled in.
  It creates a synthetic database (20k users, 200k posts, 600k likes; the schema's tables and indexes,
  none of the challenge seed) and serves 300,000 requests in the benchmark mix from memory: parsing,
  JWT checks, queries, writes and commits. The real binary is then compiled with that profile (5 to 6%
  less CPU per request on the mixed load). The training binary and database are deleted; `train.c` is
  not in the final binary.
- **SQLite settings**: WAL, `synchronous=NORMAL`, 1 GiB `mmap_size`, a 16 MB page cache, and prepared statements built once with
  `SQLITE_PREPARE_PERSISTENT`. SQLite is compiled single-threaded (`SQLITE_THREADSAFE=0`) with
  unused features left out. Queries are the reference queries; a like is one statement,
  `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT DO NOTHING`, with a post lookup only when
  nothing was inserted (to tell "already liked" from "no such post").
- **No caching** (rule 5): every request runs its query, and every token's HMAC is computed. The only
  thing computed once is the HMAC key schedule (the SHA-256 state after the padded secret), which is
  derived from `JWT_SECRET`, not from any request.
- **Server state per connection is 40 bytes** when idle (kernel socket buffers aside). Requests are read into one shared buffer; a
  connection only gets its own buffer while it holds a partial request or a response the socket
  couldn't take yet. That is what lets one process hold tens of thousands of keep-alive clients.
- **Past 65,535 clients.** Serving direct, every k6 user holds a keep-alive connection, and
  `LimitNOFILE=65535` caps descriptors per process. When this process gets within about 4,000
  descriptors of that cap, it starts a second copy of itself on the same port (`SO_REUSEPORT`), stops accepting once the copy is listening, and resumes accepting when it
  has room again. The copy does the same if it fills up. Below the cap there is exactly one process.
  Workers share the database through WAL; a write that finds its read snapshot outdated by another
  worker's commit restarts its transaction with `BEGIN IMMEDIATE`.
- **Strict input handling**: a full RFC 8259 JSON validator (duplicate keys: last wins, like
  `JSON.parse`), `\u` escapes and surrogate pairs decoded, invalid UTF-8 rejected as malformed JSON,
  whitespace trimmed with JavaScript's `String.prototype.trim` set, and the 500-character limit counted
  in Unicode code points (what SQLite's `length()` counts in the schema's `CHECK`). JWTs: HS256 only,
  constant-time signature compare, `exp` and `nbf` checked (`exp` is required).
- **HTTP/1.1 keep-alive** with no idle timeout, pipelining, `Connection: close`, HTTP/1.0, and
  `Expect: 100-continue`. Chunked request bodies aren't supported (they're out of scope).
- **No tuning outside the process** (rule 11): it only raises its own soft descriptor limit to the hard
  limit it was given.

## Local measurements

See the numbers in the pull request description. They were measured with the server container limited
to one CPU, 2 GB RAM, no swap and 65,535 descriptors, with load generators in separate containers.

## License

MIT, under the repo's [license](../../LICENSE).
