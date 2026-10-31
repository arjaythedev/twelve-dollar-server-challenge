# c-iouring-thehimalayanleo

One event-loop thread of C on raw io_uring, SQLite compiled into the same translation unit, plus a
checkpoint thread. Direct on `HOST:PORT` (`0.0.0.0:80` on the box), no Nginx.

**This entry builds on [c-iouring-shivanshthapliyal](../c-iouring-shivanshthapliyal) (#230, 270,000 users).**
Its HTTP server, io_uring plumbing, JWT and JSON code, group commit and checkpoint scheme are kept as they
are; see its README for those parts. The organizer's note on #230 says its 280k probe failed on p99 at
89 % CPU, so this entry works on CPU per request. The changes are listed below.

| | |
|---|---|
| Language | C (GCC 13 from Ubuntu 24.04, `-O2 -march=native`) |
| Framework | None. A hand-written HTTP/1.1 server on io_uring |
| io_uring | liburing 2.5 (Ubuntu 24.04 package) |
| SQLite | 3.54.1 amalgamation, pinned by SHA-256, compiled in, not patched |
| Front | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the box). No Nginx |

## What changed from #230

1. **Feed and single-post reads use SQLite's B-tree cursor API** (`src/bt.h`), as allowed by Arjay's
   2026-10-31 ruling. The server includes `sqlite3.c` so it can call `sqlite3BtreeFirst/Next/Previous`,
   `sqlite3BtreeTableMoveto` and `sqlite3BtreeIndexMoveto` inside the batch's read transaction. Every
   row comes from SQLite's pages through its pager, WAL and locks, on the request that serves it.
   - Feed: the first 20 entries of `posts_created_at_id_idx` (it is `created_at DESC, id DESC`, so these
     are the feed in order), one range scan of `likes_post_id_idx` for all 20 like counts, then each post
     row (when the next id is one lower, a `Previous` step from the last row instead of a seek from the
     root) and its author by rowid.
   - Post: one rowid seek, one index range scan for the count, one author seek.
   - No assumption about the data: ids, order and authors all come from the B-trees. Anything unusual
     (overflow payload, unexpected record shape, an id span over 1,024, a schema change) runs the plain
     SQL from #230 instead. `SQL_READS=1` turns the B-tree path off; the two paths give byte-identical
     responses (checked after writes with UTF-8 and escapes).
2. **Paced passes.** When the loop is busy it starts a new pass at most every 1 ms. Requests gather in
   the meantime, so one kernel entry per ring and one commit serve many of them. This adds at most 1 ms
   of latency.
3. **Task-work drain.** Kernels newer than the box's 6.8 run at most about 20 deferred task-work items
   per `io_uring_enter`. The loop enters a ring again while `IORING_SQ_TASKRUN` is still set. On 6.8
   this changes nothing.
4. **The checkpoint thread runs at nice 10**, so on 1 vCPU a checkpoint pass does not take CPU from
   requests (c-iouring-public #158 traced OOM stalls to this).

## Measured

On a Ryzen 9 9950X3D (kernel 7.0), the server pinned to one core and `mload` (the closed-loop user model
of `bench/load.js`, from c-iouring-moneebullah25) on four others, 30,000 users at 0.06 think-time scale
(about 39k req/s, about half a core). Server CPU per request, 5 interleaved rounds each:

| | µs/request | user µs |
|---|---:|---:|
| c-iouring-shivanshthapliyal (#230) | 10.53 ± 0.19 | 3.67 |
| this entry | 9.52 ± 0.15 | 2.77 |

In an earlier run of the same setup, c-iouring-moneebullah25 (#228, 26 µs/request on the box) measured
9.99 ± 0.14 (user 2.09) against 9.69 ± 0.20 for this entry before change 4. A WAL shim that writes each
commit with one `pwrite` was tried in the same run and dropped: 9.46 ± 0.13 µs, within noise of
this entry without it.

The machine had other jobs on it; the numbers are for comparison between entries, not a user-count
score. No user-count score is claimed.

## Rule notes

- No caches across requests. Kept between batches: prepared statements, table root pages and key
  infos (read at open), the HMAC key schedule. Every JWT is verified on every request.
- Stock SQLite: the amalgamation is unmodified. The B-tree reads use SQLite's own functions;
- WAL with `synchronous=NORMAL`; no response to a write leaves before its batch's `COMMIT` returns.
- No load shedding, nothing tuned to the load test. Configured only by `SQLITE_PATH`, `JWT_SECRET`,
  `HOST` and `PORT` (`SQL_READS=1` only switches the B-tree reads off, for testing).
