/* db/engine.h (sha256 58dd513ded2e), developed separately for this entry. */
// engine.h — the SQLite side of cursed-http.
//
// engine.c #includes sqlite3.c, so the engine and SQLite are one translation unit and the engine
// reads and writes B-trees directly (sqlite3BtreeTableMoveto / IndexMoveto / Insert), bypassing SQL.
// Build: cc -O2 -I<sqlite-amalgamation> -c engine.c   (see Makefile for the full flag set)
//
// One batch per event-loop wakeup:
//
//   eng_begin_batch(e);
//   for each parsed request:  status = eng_feed / eng_get_post / eng_like / eng_create / eng_health
//                             (each writes its exact JSON response body into the caller's buffer)
//   if (eng_commit_batch(e) != 0)  replace EVERY reply of this batch with 500 internal server error
//   send replies
//   eng_after_batch(e);       // checkpoint policy; runs after the replies are on the wire
//
// All ops of a batch run inside one SQLite transaction: a read transaction that is upgraded to a write
// transaction at the first like/create. Reads later in the batch see the batch's own earlier writes,
// so reads must be held until eng_commit_batch returns too (they may have observed rows that a failed
// commit rolls back). Nothing is cached between batches: every op reads the B-trees while it runs.
#ifndef CURSED_ENGINE_H
#define CURSED_ENGINE_H
#include <stddef.h>
#include <stdint.h>

typedef struct eng eng;

typedef struct {
  int direct_reads;      // 1: B-tree cursors (default). 0: prepared SQL (spec reference queries)
  int direct_writes;     // 1: sqlite3BtreeInsert into table + indexes. 0: prepared INSERTs (default)
  int feed_range;        // 1: contiguous-id trick: likes for the feed from one likes_post_id_idx range scan
  int walk_cells;        // 1: count likes by parsing index leaf cells in place instead of BtreeNext per like
  int feed_batch;        // 1: the feed's 20 author lookups as one batched, prefetch-interleaved multi-seek (sqlite-simd).
                         //    Ignored when mmap_size > 0 (it relies on page-cache pages)
  int exclusive;         // 1: PRAGMA locking_mode=EXCLUSIVE (WAL index in heap memory, no shm, no fcntl locks)
  int wal_coalesce;      // 1: "cursed-wal" VFS shim, one pwrite per commit instead of two per WAL frame
  int wal_fadvise;       // 1: drop checkpointed WAL frames from the page cache (POSIX_FADV_DONTNEED) and truncate the
                         //    WAL after each restart (journal_size_limit=0). Linux only
  int64_t mmap_size;     // 0: page cache only (default). >0: PRAGMA mmap_size
  int cache_kib;         // PRAGMA cache_size=-cache_kib
  int ckpt_frames;       // >0: wal_autocheckpoint=0 and eng_after_batch checkpoints at this many WAL frames.
                         //  0: SQLite's default autocheckpoint (1000 frames; only fires on the SQL write path)
  int ckpt_mode;         // SQLITE_CHECKPOINT_PASSIVE / RESTART / TRUNCATE for the manual checkpoint
  int ckpt_defer;        // 1: eng_after_batch only checkpoints past ckpt_hard; below that, eng_idle() does it
  int ckpt_hard;         // frames pending that force a checkpoint (default 4 * ckpt_frames)
  int ckpt_thread;       // 1 (default in a THREADSAFE build): checkpoint from a thread on a second connection (needs a SQLITE_THREADSAFE=2
                         //    build; uses the "unix-excl" VFS + locking_mode=NORMAL so both connections share a
                         //    heap WAL index; the main connection sleeps on SQLITE_BUSY)
  int ckpt_restart;      // ckpt_thread: WAL high mark (frames, default 32768). With wal_fadvise only the frames not yet
                         //    checkpointed (< ckpt_frames) stay in the OS page cache, not the whole WAL. Past it, eng_writes_paused()
                         //    returns 1 until the thread has copied every frame; the next write then restarts the WAL
  int ckpt_drain_ms;     // ckpt_thread: give up holding writes after this long (default 1000)
  int ckpt_drain_frames; // ckpt_thread: start holding writes only when at most this many frames are left to copy (256)
  int ckpt_async;        // 1: checkpoint from the event loop as a state machine with batched file ops ("DB_CKPT=uring";
                         //    EXCLUSIVE, no thread, no mmap). Same gate (eng_writes_paused) and marks as ckpt_thread.
                         //    The server calls eng_tick() every loop pass and can pass its io_uring via eng_set_fileops
  int ckpt_nice;         // ckpt_thread: the thread's nice value (default 0; 0-19, unprivileged threads can only raise it)
} eng_opts;

void eng_default_opts(eng_opts *o);
// Opens path (WAL, synchronous=NORMAL), applies opts, warms nothing. NULL on failure (message in err).
eng *eng_open(const char *path, const eng_opts *o, char *err, size_t errn);
void eng_close(eng *e);

// Buffer sizes that always suffice for one op's body (usernames longer than 200 bytes in the users table
// could exceed them; the op then returns 500 instead of overflowing).
#define ENG_POST_CAP 4352     // {"post":{...}} with a 500-char body escaped worst-case (\u00XX) + slack
#define ENG_FEED_CAP 86016    // {"posts":[20 posts]}

// Every op returns the HTTP status and stores the body length in *n. Error bodies
// ({"error":"post not found"}, {"error":"internal server error"}) are written too.
int eng_begin_batch(eng *e);                                         // 0 or -1
int eng_feed(eng *e, char *out, size_t cap, size_t *n);              // 200 / 500
int eng_get_post(eng *e, int64_t id, char *out, size_t cap, size_t *n);   // 200 / 404 / 500
int eng_like(eng *e, int64_t uid, int64_t pid, char *out, size_t cap, size_t *n); // 201 / 200 already / 404 / 500
// body: already JSON-decoded and trimmed (1..500 chars, checked again here against the schema CHECK);
// uname: the token's username, used as-is. id / created_at (24 bytes, not NUL-terminated) are optional outputs.
int eng_create(eng *e, int64_t uid, const char *uname, size_t un, const char *body, size_t bn,
               char *out, size_t cap, size_t *n, int64_t *id, char *created_at);
int eng_health(eng *e, int64_t uptime_s, char *out, size_t cap, size_t *n); // 200 / 503

// Feed coalescing (OFF by default; rule 5 is unresolved, see RESULTS.md). When on, eng_feed / eng_feed_body
// calls in one batch with no eng_like / eng_create call in between share one feed computation and the same
// bytes. Any like or create call (even one that fails), eng_commit_batch and a failed batch drop the shared
// result. Nothing survives the batch.
#ifndef ENG_COALESCE_DEFAULT
#define ENG_COALESCE_DEFAULT 0
#endif
void eng_set_coalesce(eng *e, int on);
// Refcounted, immutable response body: send data[0..len) to as many connections as hold a reference, then
// eng_body_unref() once per reference received. Valid after eng_commit_batch.
typedef struct eng_body { uint32_t refs, len; char data[]; } eng_body;
int eng_feed_body(eng *e, eng_body **body);   // status as eng_feed; *body is a new reference (NULL only on OOM -> 500)
void eng_body_unref(eng_body *b);
int eng_commit_batch(eng *e);    // 0: every write of the batch is committed to the WAL. -1: rolled back
void eng_after_batch(eng *e);    // runs the checkpoint policy (never inside a batch)
int eng_ckpt_due(eng *e);        // 1: the next eng_after_batch runs a checkpoint inline (submit the replies first)
void eng_idle(eng *e);           // call when the event loop has nothing to do: deferred checkpoints run here
// File ops for ckpt_async. The engine submits arrays; link=1 chains op i to op i+1 (which then runs only if op i
// succeeded in full, otherwise completes with -ECANCELED). cb runs once per op, on the loop thread. Without
// eng_set_fileops the ops run synchronously (pread/pwrite/fsync) inside the submit.
enum { ENG_FOP_READ, ENG_FOP_WRITE, ENG_FOP_FSYNC, ENG_FOP_FDATASYNC };
#define ENG_FCNTL_GETFD 0x45474644   // file control a wrapping VFS answers with the real fd (int *)
// Layout-compatible with explore/io's loop_fop (same fields, same order, same op numbers; cb's first argument is the
// loop): a server on that loop can pass `(int (*)(void *, const eng_fop *, int))loop_file_submit`-style adapters
// or simply cast the array.
typedef struct { int fd; uint8_t op, link; void *buf; uint32_t len; uint64_t off; void (*cb)(void *loop, void *ud, int res); void *ud; } eng_fop;
typedef int (*eng_fop_submit)(void *ctx, const eng_fop *ops, int n);   // 0, or -errno (nothing submitted)
void eng_set_fileops(eng *e, eng_fop_submit submit, void *ctx);
void eng_tick(eng *e);            // ckpt_async: advance the checkpoint; call once per loop pass, outside a batch
int eng_ckpt_inflight(eng *e);    // ckpt_async: file ops not yet completed; must be 0 before eng_close
void eng_ckpt_quiesce(eng *e);    // ckpt_async: start no new checkpoint cycle (the one in flight still completes; call
                                  //    eng_tick until eng_ckpt_inflight() == 0). For shutdown
void eng_set_ckpt_trace(eng *e, void (*fn)(void *ctx, int step), void *ctx);   // test hook: state transitions

// ckpt_thread / ckpt_async: 1 = hold POST requests (likes / creates) in the server's queue for now and keep serving reads.
// Check once per wakeup. Writes still work while it says 1; they only delay the WAL reset.
int eng_writes_paused(eng *e);

// JSON string body escaper (exported for tests): writes the escaped bytes of s[0..n) without quotes,
// returns bytes written. out needs 6*n+32 bytes of room. Escapes ", \ and < 0x20 like JSON.stringify.
size_t eng_json_escape(char *out, const char *s, size_t n);
size_t eng_json_escape_scalar(char *out, const char *s, size_t n);
const char *eng_simd_name(void);
// UTC "YYYY-MM-DDTHH:MM:SS.mmmZ" for unix milliseconds (exported for tests)
void eng_format_ts(char out[24], int64_t unix_ms);

// test hooks
typedef struct { uint64_t batches, write_batches, commits_failed, checkpoints, ckpt_frames, wal_writes, wal_flushes, feeds_computed, feeds_shared;
                 uint64_t busy_sleeps, restarts, wal_max, drains, drains_timed_out, feed_batch_fallbacks, wal_dropped_bytes, wal_fadvise_skipped, wal_resets, cw_late_flushes, ack_cycles, ack_errors; double ckpt_max_ms, ckpt_total_ms,
                 drain_ms_max, drain_ms_total; } eng_stats;
void eng_get_stats(eng *e, eng_stats *s);
void eng_set_clock(eng *e, int64_t (*now_ms)(void));  // override the created_at clock (differential tests)
struct sqlite3 *eng_db(eng *e);                       // the connection (only use between batches)
#endif
