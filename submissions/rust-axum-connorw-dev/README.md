# Rust + axum + SQLite

| | |
|---|---|
| Language | Rust 1.97.0, from the official standalone installer (sha256-checked, no rustup) |
| HTTP | axum 0.8 routing on hyper 1.12 and tokio 1.54, served by our own accept loop |
| SQLite | rusqlite 0.40 with SQLite 3.53.2, compiled from source by its `bundled` feature |
| JWT | HS256 with RustCrypto `hmac` 0.13 and `sha2` 0.11 |
| JSON | serde |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

447 lines of code in three files, plus 255 lines of tests. No `unsafe`, no io_uring or epoll code,
no hand-written HTTP or JSON parser. The only OS-specific code (SIGTERM handling and
`SO_REUSEADDR`) sits behind `#[cfg(unix)]`. Every dependency, transitive ones included, is pinned
in `Cargo.toml` and `Cargo.lock`.

Writes run with `synchronous=FULL`: a 201 means the row is on disk and survives a power cut, and
reads never wait for that fsync.

## Result

A five-minute hold of 64,000 users on an `s-1vcpu-2gb` droplet (Ubuntu 24.04, SYD1), with the load
coming from a second droplet in the same VPC replaying `bench/load.js`'s user loop:

| Users | Requests | Errors | p95 | p99 | Server process CPU | Server memory |
|---|---|---|---|---|---|---|
| 64,000 | 2.39 million, about 6,770 a second | 0 | 6.3 ms | 31.5 ms | about 48% | 1.0 GB |

CPU and memory are the server process's own, from `top` during the hold, so kernel time spent
outside the process isn't in them. 64,000 is as far as the benchmark goes, since that is where the
video hit Linux's connection-tracking limit. p95 is mostly the 5 ms commit window on writes, which
are 8% of requests.

## Layout

| File | What's in it |
|---|---|
| `src/main.rs` | Configuration, the runtime, and the accept loop: timeouts, `TCP_NODELAY`, listen backlog, graceful shutdown |
| `src/db.rs` | The `Db` type (pooled read connections plus a writer thread doing group commit), the `Post` type, the four queries, and tests of the writer's failure paths |
| `src/api.rs` | The axum router, the handlers, the `User` extractor (JWT), `ApiError`, and tests that drive the router |

A handler reads like the spec:

```rust
async fn like_post(
    State(app): AppState,
    user: User,
    Path(id): Path<String>,
) -> Result<(StatusCode, Json<Liked>), ApiError> {
    let post_id = post_id(&id)?;
    let (status, already_liked) = match app.db.write(move |db| db::like(db, user.id, post_id)).await? {
        Like::Added => (StatusCode::CREATED, false),
        Like::AlreadyLiked => (StatusCode::OK, true),
        Like::NoSuchPost => return Err(ApiError(StatusCode::NOT_FOUND, "post not found")),
    };
    Ok((status, Json(Liked { liked: true, already_liked, post_id })))
}
```

`User` is extracted before `Path`, which is how "check auth first, then the id" is enforced.
`db.write` resolves only after the transaction holding the write has committed.

## Running it

```bash
sudo bash install.sh   # build-essential, plus Rust 1.97.0 in /opt/rust-1.97.0
bash build.sh          # cargo build --release --locked
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
cargo test --release   # router and writer tests against temporary databases, plus JWT and parsing
```

`start.sh` raises the soft descriptor limit to the hard limit (one descriptor per keep-alive
client) and runs the binary in the foreground. `THREADS` (optional) sets the number of
event-loop threads; it defaults to the number of cores, which is one on the benchmark droplet.
The release build peaks at about 400 MB, so it fits on the 2 GB droplet.

## How it works

**Reads run on the event loop.** `Db::read` borrows a read-only connection (`query_only`, 1 GiB
`mmap`) from a small pool and runs the query on the calling thread; on one core the pool holds a
single connection. Queries are prepared once and stay compiled. Handing a 30 µs index lookup to
another thread would cost more than the lookup.

**Writes go to one writer thread** through a bounded queue (4,096 entries; when it is full,
requests wait instead of piling up in memory). The writer has its own connection with
`synchronous=FULL` and foreign keys on. It takes whatever has queued, waits 5 ms for more, runs
the batch in one `BEGIN IMMEDIATE` transaction, commits, and only then reports back to each
request. A statement that fails rolls back alone, and a panicking job loses only its own reply.
If the COMMIT fails, or SQLite rolls back the whole transaction mid-batch (disk-full, I/O,
out-of-memory and busy errors can), every request in the batch gets a 500, and jobs that haven't
run yet are skipped rather than left to commit on their own behind that 500.

The writer thread exists because of FULL: COMMIT waits for the disk, and on the event loop that
would stall every read for the length of an fsync. The second connection costs something too,
because each commit makes the readers drop their page caches. The 5 ms window makes commits rare
enough that this mostly disappears.

## Optimizations

| Change | Why |
|---|---|
| Writes batched for 5 ms per transaction | one fsync per batch, and the readers drop their page caches once per commit rather than once per write |
| Feed rows serialized straight from SQLite's buffers (`Post<&str>`) | copying 60 strings per feed into owned `String`s cost 11% of a feed's CPU |
| `-DHAVE_FDATASYNC=1` | the bundled SQLite otherwise falls back to a full `fsync` on every commit |
| SQLite built without `SQLITE_ENABLE_MEMORY_MANAGEMENT`, which the bundled build turns on | removes a global mutex taken on every page-cache access |
| `SQLITE_DEFAULT_MEMSTATUS=0` and the other options [sqlite.org recommends](https://sqlite.org/compile.html) | no allocator bookkeeping on every SQLite `malloc` |
| Serving direct, no Nginx | Nginx's `worker_connections 16384` caps the score near 16,000 users |
| `TCP_NODELAY` and a listen backlog of 4,096 | a feed is four TCP segments; the backlog absorbs the ~1,000 new connections per second of a 64,000-user ramp |
| HMAC key schedule computed once at startup | every request still verifies its signature |
| Body limit and upload timeout only on `POST /posts` | other routes don't pay for them |

## Production notes

- SIGTERM or Ctrl-C stops accepting, lets in-flight requests finish (10 s at most), and exits.
- Timeouts and limits: 75 s header and keep-alive idle timeout, 10 s to upload a body, 16 KiB
  body cap, 4,096-entry write queue. The spec asks for at least 65 s of keep-alive, and k6 only
  retries GETs when the server has closed a keep-alive connection, so a shorter timeout would turn
  into failed POSTs.
- `accept()` failures such as EMFILE back off for 100 ms instead of spinning.
- Reader connections are `query_only`. The writer enforces the schema's foreign keys, so a validly
  signed token for a user id that isn't in `users` gets a 401 on writes instead of creating an
  orphan post the feed would never show.
- `/health` answers 503 if the writer thread has stopped, not only when a read fails.
- SQLite's automatic WAL checkpoint runs on the writer thread, so it delays writes, never reads.
- A batch holds up to 1,024 writes, enough to keep up with 500 writes a second through a
  two-second disk stall.
- Unknown paths get `{"error":"not found"}`; a wrong method on a known path gets axum's 405.
- Memory: about 16 KB per keep-alive connection, 1.0 GB at 64,000 users.
- Logs go to stderr: startup, shutdown and any SQLite error.

## Tests

- `bash test/run.sh submissions/<this folder>`: 42/42.
- `cargo test --release`, against temporary databases:
  - the router driven with `oneshot`: reads, writes visible once acknowledged, every 400/401/404
    case in the spec, and tokens for a user that doesn't exist;
  - the writer's failure paths, which the video flagged as group commit's cost: a statement that
    fails rolls back alone while the rest of its batch commits; a COMMIT that fails fails every
    write in the batch, and the writer carries on; after SQLite rolls back the whole transaction,
    the writes behind it in the batch never run; a panicking write loses only its own reply; and
    while another process holds the write lock, writes fail after the 5 s busy timeout instead of
    hanging;
  - JWT edge cases (wrong key, `alg: none`, `HS512`, expired, missing `exp`, malformed), id parsing
    and trimming.

## License

MIT, under the repo's [license](../../LICENSE).
