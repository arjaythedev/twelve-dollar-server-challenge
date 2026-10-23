#include <string.h>
#include "sqlite/sqlite3.h"

typedef struct {
	long long ints[4];
	int used;
	int changes;
	int nowait;
} tq_io;

// tq_run binds io->ints[0..ni) to ?1..?ni and, when tlen >= 0, text to ?ni+1. It steps the
// statement to completion and packs every row into out, one column per spec character:
// 'i' is an 8-byte native int64, 't' is a 4-byte native length followed by the bytes.
// Returns the row count, -rc on an SQLite error, or TQ_FULL when out is too small.
#define TQ_FULL (-100000)

static int tq_run(sqlite3_stmt *s, tq_io *io, int ni, const char *text, int tlen,
                  const char *spec, unsigned char *out, int cap) {
	int rc, rows = 0, pos = 0, nc = (int)strlen(spec);
	for (int i = 0; i < ni; i++) sqlite3_bind_int64(s, i + 1, io->ints[i]);
	if (tlen >= 0) sqlite3_bind_text(s, ni + 1, text, tlen, SQLITE_STATIC);
	while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
		for (int c = 0; c < nc; c++) {
			if (spec[c] == 'i') {
				long long v = sqlite3_column_int64(s, c);
				if (pos + 8 > cap) goto full;
				memcpy(out + pos, &v, 8);
				pos += 8;
			} else {
				const unsigned char *t = sqlite3_column_text(s, c);
				int n = sqlite3_column_bytes(s, c);
				if (pos + 4 + n > cap) goto full;
				memcpy(out + pos, &n, 4);
				if (n) memcpy(out + pos + 4, t, n);
				pos += 4 + n;
			}
		}
		rows++;
	}
	io->used = pos;
	io->changes = sqlite3_changes(sqlite3_db_handle(s));
	sqlite3_reset(s);
	return rc == SQLITE_DONE ? rows : -rc;
full:
	sqlite3_reset(s);
	return TQ_FULL;
}

// tq_busy is an event loop's busy handler. While io->nowait is set (around BEGIN IMMEDIATE) it
// gives up at once, so a held write lock defers the batch's writes instead of stalling the loop.
// Anything else that meets a lock (rare, WAL bookkeeping) yields the one CPU to the holder first,
// then sleeps briefly, and gives up after about 5 s.
#include <sched.h>
#include <unistd.h>
static int tq_busy(void *arg, int n) {
	tq_io *io = (tq_io *)arg;
	if (io->nowait) return 0;
	if (n < 1000) { sched_yield(); return 1; }
	if (n < 1000 + 50000) { usleep(100); return 1; }
	return 0;
}

static void tq_set_busy(sqlite3 *db, tq_io *io) { sqlite3_busy_handler(db, tq_busy, io); }

// tq_try_begin runs BEGIN IMMEDIATE without waiting: SQLITE_DONE, or SQLITE_BUSY to retry later.
static int tq_try_begin(sqlite3_stmt *s, tq_io *io) {
	io->nowait = 1;
	int rc = sqlite3_step(s);
	io->nowait = 0;
	sqlite3_reset(s);
	return rc;
}

// tq_checkpoint copies every WAL frame into the database and waits for readers to leave the WAL,
// so the next writer starts the WAL file over from the beginning. A PASSIVE checkpoint cannot
// do that under a steady write load: frames arrive faster than it finishes, so the WAL never
// rewinds and every page lookup searches a wal-index that grows without bound.
static int tq_checkpoint(sqlite3 *db) {
	return sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_RESTART, NULL, NULL);
}
