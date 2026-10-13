package main

/*
#cgo CFLAGS: -I${SRCDIR}/.deps
#cgo LDFLAGS: -L${SRCDIR}/.deps -lsqlite3 -lm
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static int bind_text(sqlite3_stmt *s, int i, const char *p, int n) {
	return sqlite3_bind_text(s, i, p, n, SQLITE_TRANSIENT);
}

// Steps every row of s and packs each column as a uint32 length in native byte order followed by its
// text, copying only while the rows fit in buf. *n is the size of all rows. Resets s.
static int step_rows(sqlite3_stmt *s, char *buf, int cap, int *n) {
	int rc, used = 0, cols = sqlite3_column_count(s);
	while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
		for (int i = 0; i < cols; i++) {
			const unsigned char *text = sqlite3_column_text(s, i);
			uint32_t len = (uint32_t)sqlite3_column_bytes(s, i);
			if (used + 4 + (int)len <= cap) {
				memcpy(buf + used, &len, 4);
				memcpy(buf + used + 4, text, len);
			}
			used += 4 + (int)len;
		}
	}
	sqlite3_reset(s);
	*n = used;
	return rc;
}

static int wal_pages;

static int on_wal(void *arg, sqlite3 *db, const char *name, int pages) {
	wal_pages = pages;
	return SQLITE_OK;
}

// Replaces SQLite's checkpoint inside COMMIT, so the server can checkpoint after it sends the batch.
static void watch_wal(sqlite3 *db) { sqlite3_wal_hook(db, on_wal, 0); }

static int wal_size(void) { return wal_pages; }

static int checkpoint(sqlite3 *db) {
	wal_pages = 0;
	return sqlite3_wal_checkpoint_v2(db, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0);
}
*/
import "C"

import (
	"errors"
	"slices"
	"unsafe"
)

// postSelect returns the columns appendPosts expects: id, body, created_at, author, like_count.
const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id`

// Exclusive locking before the first read keeps the WAL index in heap memory and skips the file locks on every transaction.
// A variable so that tests/commitfail.sh can build with other pragmas via -ldflags "-X 'main.pragmas=...'".
var pragmas = "PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;"

// db is one SQLite connection. Only the event loop goroutine uses it.
type db struct {
	conn                                     *C.sqlite3
	health, feed, post, insert, like, exists *C.sqlite3_stmt
	begin, commit, rollback                  *C.sqlite3_stmt
	inTx                                     bool   // a batch transaction is open
	failed                                   bool   // SQLite rolled the batch transaction back
	packed                                   []byte // rows from step_rows, reused across queries
}

var errBatchRolledBack = errors.New("batch transaction was rolled back")

func openDB(path string) (*db, error) {
	d := &db{}
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.sqlite3_open_v2(cpath, &d.conn, C.SQLITE_OPEN_READWRITE, nil) != C.SQLITE_OK {
		return nil, d.err()
	}
	if err := d.exec(pragmas); err != nil {
		return nil, err
	}
	for _, p := range []struct {
		stmt **C.sqlite3_stmt
		sql  string
	}{
		{&d.health, "SELECT 1"},
		{&d.feed, postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"},
		{&d.post, postSelect + " WHERE p.id = ?"},
		{&d.insert, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"},
		// Inserts only when the post exists, so no change means a repeat or a missing post.
		{&d.like, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING"},
		{&d.exists, "SELECT 1 FROM posts WHERE id = ?"},
		{&d.begin, "BEGIN IMMEDIATE"},
		{&d.commit, "COMMIT"},
		{&d.rollback, "ROLLBACK"},
	} {
		var err error
		if *p.stmt, err = d.prepare(p.sql); err != nil {
			return nil, err
		}
	}
	C.watch_wal(d.conn)
	return d, nil
}

func (d *db) prepare(sql string) (*C.sqlite3_stmt, error) {
	var s *C.sqlite3_stmt
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if C.sqlite3_prepare_v3(d.conn, csql, -1, C.SQLITE_PREPARE_PERSISTENT, &s, nil) != C.SQLITE_OK {
		return nil, d.err()
	}
	return s, nil
}

func (d *db) err() error { return errors.New(C.GoString(C.sqlite3_errmsg(d.conn))) }

func (d *db) exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if C.sqlite3_exec(d.conn, csql, nil, nil, nil) != C.SQLITE_OK {
		return d.err()
	}
	return nil
}

func (d *db) ping() error {
	rc := C.sqlite3_step(d.health)
	C.sqlite3_reset(d.health)
	if rc != C.SQLITE_ROW {
		return d.err()
	}
	return nil
}

// feedRows returns the newest posts packed by step_rows, valid until the next query.
func (d *db) feedRows() ([]byte, error) { return d.rows(d.feed) }

// postRows returns one post packed by step_rows, or nothing if it does not exist.
func (d *db) postRows(id int64) ([]byte, error) {
	C.sqlite3_bind_int64(d.post, 1, C.sqlite3_int64(id))
	return d.rows(d.post)
}

// rows runs s with one cgo call and returns every row packed by step_rows.
func (d *db) rows(s *C.sqlite3_stmt) ([]byte, error) {
	d.packed = slices.Grow(d.packed[:0], 8<<10)
	for {
		buf := d.packed[:cap(d.packed)]
		var n C.int
		if C.step_rows(s, (*C.char)(unsafe.Pointer(&buf[0])), C.int(len(buf)), &n) != C.SQLITE_DONE {
			return nil, d.err()
		}
		if int(n) <= len(buf) {
			return buf[:n], nil
		}
		d.packed = slices.Grow(d.packed[:0], int(n)) // rerun with room for every row
	}
}

// columnText views a text column. The bytes are valid until the next step or reset.
func columnText(s *C.sqlite3_stmt, i C.int) []byte {
	p := C.sqlite3_column_text(s, i)
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(C.sqlite3_column_bytes(s, i)))
}

// run steps a statement that returns no rows and resets it.
func (d *db) run(s *C.sqlite3_stmt) error {
	rc := C.sqlite3_step(s)
	C.sqlite3_reset(s)
	if rc != C.SQLITE_DONE {
		return d.err()
	}
	return nil
}

// beginWrite opens the batch transaction before the batch's first write.
func (d *db) beginWrite() error {
	switch {
	case d.failed:
		return errBatchRolledBack
	case d.inTx:
		return nil
	}
	if err := d.run(d.begin); err != nil {
		return err
	}
	d.inTx = true
	return nil
}

// writeFailed notes whether a failed statement took the whole batch transaction with it.
func (d *db) writeFailed() {
	if C.sqlite3_get_autocommit(d.conn) != 0 {
		d.failed = true
	}
}

// checkpointIfDue copies the WAL into the database once it holds 1,000 pages, SQLite's default threshold.
func (d *db) checkpointIfDue() error {
	if C.wal_size() < 1000 {
		return nil
	}
	if C.checkpoint(d.conn) != C.SQLITE_OK {
		return d.err()
	}
	return nil
}

// commitBatch commits the batch transaction. On error none of the batch's writes are kept.
func (d *db) commitBatch() error {
	if !d.inTx {
		return nil
	}
	failed := d.failed
	d.inTx, d.failed = false, false
	if failed {
		return errBatchRolledBack
	}
	if err := d.run(d.commit); err != nil {
		_ = d.run(d.rollback)
		return err
	}
	return nil
}

// insertPost writes a new post in the batch transaction and returns its id and created_at.
func (d *db) insertPost(userID int64, body string) (int64, string, error) {
	if err := d.beginWrite(); err != nil {
		return 0, "", err
	}
	s := d.insert
	defer C.sqlite3_reset(s)
	C.sqlite3_bind_int64(s, 1, C.sqlite3_int64(userID))
	C.bind_text(s, 2, (*C.char)(unsafe.Pointer(unsafe.StringData(body))), C.int(len(body)))
	if C.sqlite3_step(s) != C.SQLITE_ROW {
		err := d.err()
		d.writeFailed()
		return 0, "", err
	}
	id, created := int64(C.sqlite3_column_int64(s, 0)), string(columnText(s, 1))
	if C.sqlite3_step(s) != C.SQLITE_DONE {
		err := d.err()
		d.writeFailed()
		return 0, "", err
	}
	return id, created, nil
}

// likePost writes a like in the batch transaction and reports whether it was new and whether the post exists.
func (d *db) likePost(userID, postID int64) (inserted, exists bool, err error) {
	if err := d.beginWrite(); err != nil {
		return false, false, err
	}
	s := d.like
	C.sqlite3_bind_int64(s, 1, C.sqlite3_int64(userID))
	C.sqlite3_bind_int64(s, 2, C.sqlite3_int64(postID))
	if err := d.run(s); err != nil {
		d.writeFailed()
		return false, false, err
	}
	if C.sqlite3_changes(d.conn) == 1 {
		return true, true, nil
	}
	C.sqlite3_bind_int64(d.exists, 1, C.sqlite3_int64(postID))
	rc := C.sqlite3_step(d.exists)
	C.sqlite3_reset(d.exists)
	switch rc {
	case C.SQLITE_ROW:
		return false, true, nil
	case C.SQLITE_DONE:
		return false, false, nil
	}
	return false, false, d.err()
}
