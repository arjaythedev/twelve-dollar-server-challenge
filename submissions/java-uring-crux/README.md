# Java 27 / io_uring / SQLite

Direct HTTP/1.1 on `$HOST:$PORT`. No Nginx, no framework, no JDBC.

| | |
|---|---|
| Language | Java 27: OpenJDK `27.0.1.0.3` from [jdk.java.net/27](https://jdk.java.net/27/), sha256-pinned in `install.sh` |
| Framework | None. Linux `io_uring` through the Foreign Function and Memory API (`java.lang.foreign`). `java.nio` is the macOS path and the fallback if `io_uring_setup` is refused |
| SQLite | 3.54.0 amalgamation, compiled by `build.sh`, called through FFM |
| JSON | Create, and a feed whose ids span 256 or more, use SQLite `json_object`. The usual feed and `GET /posts/:id` format that same object inside the SQLite library, straight into the send buffer. Request bodies and JWTs use a small strict parser |
| **Nginx or direct** | **Direct**: the process binds `HOST` and `PORT` itself (`0.0.0.0:80` on the benchmark) |

## What is in the submission

`install.sh`, `build.sh`, `start.sh`, this README, `.gitignore`, and `src/`. No build output. A pull request should add only `submissions/java-uring-crux/`. CI runs `install.sh` and `test/run.sh` for every submission a pull request changes.

Linux uses io_uring. macOS, and a Linux host that refuses `io_uring_setup`, use `java.nio`. The process is configured only by `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT`.

## Why this shape

The scored workload is tens of thousands of idle keep-alive connections and about 0.1 requests per user per second, almost all reads. The core saturates on SQLite and on a system call per socket. The pieces aimed at that:

- **Fixed files, eight rings.** Each ring registers its own file table (up to 65535) and its own listen socket (`SO_REUSEPORT`). An accepted socket lives in that table, so the process file-descriptor limit is not one connection per descriptor. A multishot receive stays armed on the socket and borrows a provided buffer only for the bytes that just arrived, so an idle connection holds no buffer and a read is one completion. The response is built in an off-heap slot and sent from that address. The submission queue tracks the kernel head, and a full socket buffer retries the send on `POLLOUT` instead of dropping the response.
- **One thread.** The same thread submits the ring and runs SQLite. A second GC or worker thread would share the single core. `start.sh` selects Serial GC and a fixed heap. Compact object headers are on (the JDK 27 default; the flag keeps them on JDK 25).
- **One deferred transaction.** Read-only batches leave it open, so a saturated read loop does not commit. A batch that writes commits before any of its responses are queued.
- **Feed by id range.** The 20 newest ids are loaded every request. When they span fewer than 256 ids, like counts are one `BETWEEN` scan, the columns are one primary-key read, and the JSON is written in that same library call. A wider span uses the original join.
- **SQLite cache.** `cache_size=500` pages, `mmap_size=1GiB`, `locking_mode=EXCLUSIVE`, `synchronous=NORMAL`, `wal_autocheckpoint=0`. A restart checkpoint runs between batches after 4096 commits.
- **JWT.** Every authenticated request is verified as HS256. A token with no `exp`, a bad signature, or any other algorithm is rejected. Tokens are not remembered.

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

From the repository root, after `bash seed/make-seed.sh`:

```bash
bash test/run.sh submissions/java-uring-crux
```
