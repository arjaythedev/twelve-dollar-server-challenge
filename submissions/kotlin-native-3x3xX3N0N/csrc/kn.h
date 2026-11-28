/* The C side of the Kotlin/Native server, as seen by Kotlin through cinterop (kn.def).
 * Kotlin owns the event loop, HTTP, routing, JWT and JSON; C owns the kernel plumbing (io_uring through liburing)
 * and SQLite (the stock amalgamation compiled into db.c, plus B-tree read paths in btree_reads.c). */
#ifndef KN_H
#define KN_H
#include <stdint.h>

/* ------------------------------------------------------------------ database (db.c) */
/* Opens the reader, writer and checkpointer connections. vfs: NULL = default "unix", or e.g. "unix-excl".
 * btree: 1 = serve GET /feed and GET /posts/:id from B-tree cursors (SQL as the fallback), 0 = SQL only.
 * Returns 0, or -1 with a message in err. */
int db_open(const char *path, const char *vfs, int btree, char *err, int errcap);
/* SELECT 1 on the reader. 0 = ok, else -1 with SQLite's message in err. */
int db_health(char *err, int errcap);
/* {"posts":[...]} into out; returns its length, or -1 (error / out too small). */
int db_feed(uint8_t *out, int cap);
/* {"post":{...}} into out; returns its length, 0 = no such post, -1 = error. */
int db_post(int64_t id, uint8_t *out, int cap);
/* Group commit. By default it runs on the loop, on the reader connection (keeps the reader's page cache): the loop
 * stages a batch and db_wq_start commits it at once (returns 2: results ready) or, if a checkpoint holds the write
 * lock, at a later db_pass_end (returns 1 then). With KN_COMMITTER=1, or for a while after a slow commit on the loop
 * (slow-disk guard, KN_SLOW_MS), the batch goes to the committer thread (own connection) instead: db_wq_start returns
 * 0 and the thread writes to the eventfd returned by db_wq_init (watched by ring 0: an EV_WAKE event) when it is done.
 * Either way the loop answers the writes only after COMMIT has returned. */
int db_wq_init(void);   /* starts the committer thread; returns the eventfd it signals (watch it on ring 0), or -1 */
int db_wq_add_like(int64_t uid, int64_t pid);
int db_wq_add_post(int64_t uid, const uint8_t *body, int blen, const uint8_t *author, int alen);
int db_wq_start(void);       /* 2 = committed on the loop (results ready), 0 = started/pending, 1 = a batch is still in flight */
int db_wq_done(void);        /* >= 0: the in-flight batch is committed (or failed) and has this many results; -1 = not yet */
/* Result i of the finished batch: post: 201 + JSON; like: 1 new / 0 repeat / -1 no such post; -2 error (and the
 * whole batch is -2 if its COMMIT failed). For posts the JSON is copied to out; returns its length. */
int db_wq_result(int i, int *code, uint8_t *out, int cap);
void db_wq_release(void);    /* results read: the reader drops its snapshot (the next read sees the commit) */
/* End of a loop pass: retries a pending on-loop commit (returns 1 if it is now committed: read the results), and
 * drops the reader's snapshot if the checkpointer is waiting for readers to move on. */
int db_pass_end(void);
/* Compares the B-tree paths with the SQL paths, byte for byte, for every post id in [1, max id + 10] and the feed.
 * Returns the number of mismatches (details on stderr). */
int db_verify(void);
/* Starts the checkpointer thread (own connection, nice 5). */
int ckpt_start(void);
/* Counters for the stats line: B-tree hits, SQL fallbacks. */
int64_t db_stat(int which);

/* ------------------------------------------------------------------ transport (uring.c) */
#define EV_DATA 1
#define EV_OPEN 2
#define EV_CLOSED 3
#define EV_WAKE 4    /* the eventfd from ur_watch_eventfd fired */
typedef struct kn_ev {
    int32_t kind;
    int32_t slot;   /* ring * 65536 + direct-descriptor index */
    int32_t gen;    /* connection generation of this slot */
    int32_t len;    /* EV_DATA: bytes at ptr (a provided buffer, valid until ur_done) */
    uint8_t *ptr;
} kn_ev;

/* Listener on host:port, up to max_rings rings of one direct-descriptor table each (a table is capped at
 * RLIMIT_NOFILE, so more rings hold more connections in one process). Returns 0, or -1 (errno on stderr). */
int ur_init(const char *host, int port, int max_rings);
/* Submits everything queued, waits until at least wait_nr completions are ready or timeout_us passes
 * (timeout_us < 0: no timeout, wait_nr ignored = 1), and turns completions into events. Returns the event count. */
int ur_poll(kn_ev *evs, int max, int wait_nr, int64_t timeout_us);
/* Returns the provided buffers of the last ur_poll and re-arms (bounded) receives that ran out of buffers. */
void ur_done(void);
/* Queues len bytes for the connection (copied). Ignored if the connection is gone (gen mismatch). */
void ur_send(int slot, int gen, const uint8_t *p, int len);
/* Closes the connection once its queued output is sent. */
void ur_close_after(int slot, int gen);
/* Watch an eventfd on ring 0: each write to it becomes one EV_WAKE event (re-armed automatically). */
int ur_watch_eventfd(int fd);
/* Open connections and other counters for the stats line. */
int64_t ur_stat(int which);

/* CLOCK_MONOTONIC in ns, and the wall clock in seconds (JWT exp/nbf). */
int64_t kn_now_ns(void);
int64_t kn_wall_s(void);
#endif
