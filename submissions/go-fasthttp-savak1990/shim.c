#include <string.h>
#include "shim.h"

int shim_query(sqlite3_stmt *s, sqlite3_int64 i1, sqlite3_int64 i2,
               const char *t1, int n1, const char *t2, int n2,
               unsigned char *out, int cap) {
	int idx = 1, rc, n = 0;
	sqlite3_bind_int64(s, idx++, i1);
	if (i2 >= 0) sqlite3_bind_int64(s, idx++, i2);
	if (n1 >= 0) sqlite3_bind_text(s, idx++, t1 ? t1 : "", n1, SQLITE_STATIC);
	if (n2 >= 0) sqlite3_bind_text(s, idx++, t2 ? t2 : "", n2, SQLITE_STATIC);
	rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(s, 0);
		n = sqlite3_column_bytes(s, 0);
		if (n <= cap) memcpy(out, t, n);
		rc = sqlite3_step(s);
	}
	sqlite3_reset(s);
	sqlite3_clear_bindings(s);
	return rc == SQLITE_DONE ? n : -rc;
}

int shim_exec(sqlite3_stmt *s, sqlite3_int64 i1, sqlite3_int64 i2) {
	sqlite3_bind_int64(s, 1, i1);
	sqlite3_bind_int64(s, 2, i2);
	int rc = sqlite3_step(s);
	sqlite3_reset(s);
	if (rc != SQLITE_DONE) return -rc;
	return sqlite3_changes(sqlite3_db_handle(s));
}

static int wal_frames;
static int wal_hook(void *arg, sqlite3 *db, const char *name, int frames) {
	(void)arg; (void)db; (void)name;
	wal_frames = frames;
	return SQLITE_OK;
}
int shim_wal_frames(void) { return wal_frames; }
void shim_install_wal_hook(sqlite3 *db) { sqlite3_wal_hook(db, wal_hook, NULL); }

int shim_checkpoint(sqlite3 *db) {
	int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
	if (rc == SQLITE_OK) wal_frames = 0;
	return rc;
}

#ifdef SHIM_GCOV
extern void __gcov_dump(void);
void shim_gcov_dump(void) { __gcov_dump(); }
#else
void shim_gcov_dump(void) {}
#endif
