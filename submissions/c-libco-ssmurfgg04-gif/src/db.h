/* db.h - SQLite layer: statements, feed/post/like/create/health
 *
 * c-libco runs one single-threaded event loop per process, so one SQLite
 * connection per process is correct by construction: coroutines are
 * cooperative and never yield inside a transaction, reads and writes are
 * serialized without locks, and a commit always happens before the
 * responses of that request are sent. Multiple processes share the
 * database through WAL.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void db_open(const char *path);
void db_close(void);

/* No-op in this design: the single-threaded event loop owns the one
 * connection. Kept for interface parity with the io_uring submission. */
void db_thread_init(void);

/* Instrumentation hook, called per handled request. */
void db_req_note(long long us);

/* Passive checkpoint + WAL size, driven by the stats coroutine. */
void db_checkpoint(void);
long long db_wal_bytes(void);

/* Transaction state visible to the response-sending path. */
int db_txn_begin(void);
int db_commit(void);   /* 0 failed (already rolled back), 1 ok */
int db_in_txn(void);

void h_health(int slot);
void h_feed(int slot);
void h_post(int slot, int64_t id);
void h_like(int slot, int64_t uid, int64_t pid);
void h_create(int slot, int64_t uid, const char *uname, size_t ulen,
              const char *body, size_t blen);

#ifdef __cplusplus
}
#endif
