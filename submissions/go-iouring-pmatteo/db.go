package main

/*
// The SQLite amalgamation is not compiled here. build.sh compiles it on its own into
// .deps/libsqlite3.a so it can be built twice, once instrumented and once with the profile that
// run produced, which cgo's temporary object paths make impossible. sqlite_cfg.h is what keeps
// the two compilations agreeing.
#cgo CFLAGS: -I${SRCDIR} -I${SRCDIR}/.deps -std=c99 -Wno-unused-function
#cgo LDFLAGS: ${SRCDIR}/.deps/libsqlite3.a -lm
#include <stdlib.h>
#include "sqlite_cfg.h"
#include "sqlite3.h"

// SQLITE_TRANSIENT is ((sqlite3_destructor_type)-1), which cgo cannot express as a Go constant.
static void bind_text_transient(sqlite3_stmt *s, int i, const char *v, int n) {
  sqlite3_bind_text(s, i, v, n, SQLITE_TRANSIENT);
}

// SQLITE_TRANSIENT again, for a body that cannot be stored as text (see bindBody).
static void bind_blob_transient(sqlite3_stmt *s, int i, const void *v, int n) {
  sqlite3_bind_blob(s, i, v, n, SQLITE_TRANSIENT);
}

// PASSIVE never waits: it reports SQLITE_BUSY rather than blocking on a reader, which is what
// makes it safe on the loop thread.
static int wal_checkpoint_passive(sqlite3 *h, int *log, int *ckpt) {
  return sqlite3_wal_checkpoint_v2(h, NULL, SQLITE_CHECKPOINT_PASSIVE, log, ckpt);
}

// col_text_n returns a text column and its length together. Reading them as two cgo calls cost
// two crossings per column, and a rendered post has three text columns: sixty crossings per feed
// where thirty do.
static const unsigned char *col_text_n(sqlite3_stmt *s, int i, int *n) {
  const unsigned char *p = sqlite3_column_text(s, i);
  *n = sqlite3_column_bytes(s, i);
  return p;
}
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// db holds the one connection and the prepared statements. Entered only from the loop thread,
// which is what keeps SQLITE_THREADSAFE=0 valid: that build option removes every mutex from
// SQLite, so a second thread entering it would be undefined behaviour rather than slow.
type db struct {
	h *C.sqlite3

	feed       *C.sqlite3_stmt
	feedIDs    *C.sqlite3_stmt
	feedRange  *C.sqlite3_stmt
	likesRange *C.sqlite3_stmt
	post       *C.sqlite3_stmt
	postExists *C.sqlite3_stmt
	insertPost *C.sqlite3_stmt
	insertLike *C.sqlite3_stmt
	ping       *C.sqlite3_stmt
}

const (
	sqlPostColumns = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id`

	sqlFeed = sqlPostColumns + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20`

	// The range-scan feed, in three statements instead of one.
	//
	// sqlFeed costs twenty correlated count(*) subqueries, each an index seek into likes, plus
	// twenty rowid lookups into posts scattered by the index order. The twenty newest posts are
	// almost always the twenty highest ids, so when they are contiguous the same answer comes
	// out of one index-only scan for the ids, one sequential rowid range over posts, and one
	// index range over likes that is counted in memory. Three seeks instead of forty.
	//
	// Only when the ids come back contiguous and strictly descending, which api.go checks: a
	// jittered created_at could order them otherwise, and then sqlFeed is still the answer.
	sqlFeedIDs = `SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20`

	sqlFeedRange = `SELECT p.id, p.body, p.created_at, u.username
  FROM posts p JOIN users u ON u.id = p.user_id
 WHERE p.id BETWEEN ?1 AND ?2 ORDER BY p.id DESC`

	// count(*) is grouped here rather than tallied in Go: the ungrouped form returns one row per
	// like, about four per post, so twenty posts cost roughly eighty steps and eighty column
	// reads to produce twenty integers. Grouped, it is at most twenty rows. likes_post_id_idx
	// covers both the range and the grouping, so the plan is the same index scan either way.
	sqlLikesRange = `SELECT post_id, count(*) FROM likes
 WHERE post_id BETWEEN ?1 AND ?2 GROUP BY post_id`
	sqlPost = sqlPostColumns + ` WHERE p.id = ?1`

	sqlPostExists = `SELECT 1 FROM posts WHERE id = ?1`
	sqlInsertPost = `INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at`
	sqlInsertLike = `INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING`
	sqlPing       = `SELECT 1`
)

// stmt names a prepared statement outside this file, where the cgo types are not in scope.
type stmt = *C.sqlite3_stmt

func openDB(path string) (*db, error) {
	return openDBWith(path, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_NOMUTEX, "")
}

// openDBCreate makes a database and applies a schema to it. Only the training build uses it: the
// server always runs against the seed database, which already exists.
func openDBCreate(path, schema string) (*db, error) {
	return openDBWith(path,
		C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_CREATE|C.SQLITE_OPEN_NOMUTEX, schema)
}

func openDBWith(path string, flags C.int, schema string) (*db, error) {
	if rc := C.sqlite3_initialize(); rc != C.SQLITE_OK {
		return nil, fmt.Errorf("sqlite3_initialize: %d", rc)
	}

	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	d := &db{}
	rc := C.sqlite3_open_v2(cpath, &d.h, flags, nil)
	if rc != C.SQLITE_OK {
		return nil, fmt.Errorf("open %s: %s", path, d.errmsg())
	}

	// wal_autocheckpoint=0 because an automatic checkpoint runs inside whichever request happens
	// to cross the threshold, which puts a multi-megabyte WAL copy in one client's latency.
	// engine.checkpointWAL runs it once a tick instead, and nothing else bounds the log.
	for _, p := range []string{
		"PRAGMA journal_mode=WAL",
		"PRAGMA synchronous=NORMAL",
		"PRAGMA wal_autocheckpoint=0",
		"PRAGMA temp_store=MEMORY",
		"PRAGMA cache_size=-8192",
		"PRAGMA busy_timeout=5000",
	} {
		if err := d.exec(p); err != nil {
			return nil, fmt.Errorf("%s: %w", p, err)
		}
	}

	if schema != "" {
		if err := d.exec(schema); err != nil {
			return nil, fmt.Errorf("apply schema: %w", err)
		}
	}

	for _, s := range []struct {
		sql  string
		dest **C.sqlite3_stmt
	}{
		{sqlFeed, &d.feed},
		{sqlFeedIDs, &d.feedIDs},
		{sqlFeedRange, &d.feedRange},
		{sqlLikesRange, &d.likesRange},
		{sqlPost, &d.post},
		{sqlPostExists, &d.postExists},
		{sqlInsertPost, &d.insertPost},
		{sqlInsertLike, &d.insertLike},
		{sqlPing, &d.ping},
	} {
		if err := d.prepare(s.sql, s.dest); err != nil {
			return nil, err
		}
	}
	return d, nil
}

func (d *db) errmsg() string { return C.GoString(C.sqlite3_errmsg(d.h)) }

func (d *db) exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if rc := C.sqlite3_exec(d.h, csql, nil, nil, nil); rc != C.SQLITE_OK {
		return errors.New(d.errmsg())
	}
	return nil
}

func (d *db) prepare(sql string, dest **C.sqlite3_stmt) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	rc := C.sqlite3_prepare_v3(d.h, csql, C.int(len(sql)), C.SQLITE_PREPARE_PERSISTENT, dest, nil)
	if rc != C.SQLITE_OK {
		return fmt.Errorf("prepare %q: %s", sql, d.errmsg())
	}
	return nil
}

func bindInt(s *C.sqlite3_stmt, i int, v int64) {
	C.sqlite3_bind_int64(s, C.int(i), C.sqlite3_int64(v))
}

// bindText passes a Go slice straight to SQLite. SQLITE_TRANSIENT makes SQLite copy it before
// returning, so no Go pointer is retained across the call.
func bindText(s *C.sqlite3_stmt, i int, v []byte) {
	if len(v) == 0 {
		C.bind_text_transient(s, C.int(i), nil, 0)
		return
	}
	C.bind_text_transient(s, C.int(i), (*C.char)(unsafe.Pointer(&v[0])), C.int(len(v)))
}

// checkpoint copies the log back into the database file and reports the frames it moved. It is
// the only thing bounding the log: wal_autocheckpoint is 0. SQLITE_BUSY is normal, not an error -
// PASSIVE gives up rather than waiting, and the next tick retries.
func (d *db) checkpoint() (int, error) {
	var nLog, nCkpt C.int
	rc := C.wal_checkpoint_passive(d.h, &nLog, &nCkpt)
	if rc == C.SQLITE_BUSY {
		return 0, nil
	}
	if rc != C.SQLITE_OK {
		return 0, fmt.Errorf("wal_checkpoint: %s", d.errmsg())
	}
	if nCkpt < 0 {
		return 0, nil // no WAL on this connection
	}
	return int(nCkpt), nil
}

// bindBody binds a post body, as text except when it starts with a NUL.
//
// length() counts characters up to the first NUL, so such a body is text that posts.body's
// CHECK (length(body) BETWEEN 1 AND 500) can never accept, however short it is. As a blob the
// same bytes are counted as bytes, which the CHECK accepts, and they are stored and read back
// byte for byte. The spec answers 201 for any non-empty body of at most 500 characters, and this
// is the only encoding that can hold one. A body over 500 bytes still fails the CHECK and falls
// to the spec's 500.
func bindBody(s *C.sqlite3_stmt, i int, v []byte) {
	if len(v) > 0 && v[0] == 0 {
		C.bind_blob_transient(s, C.int(i), unsafe.Pointer(&v[0]), C.int(len(v)))
		return
	}
	bindText(s, i, v)
}

// colText returns a view over SQLite's own buffer. It is valid only until the next step or reset
// on this statement, so callers must copy it out (append) before stepping again.
func colText(s *C.sqlite3_stmt, i int) []byte {
	var n C.int
	p := C.col_text_n(s, C.int(i), &n)
	if p == nil || n == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n))
}

func colInt(s *C.sqlite3_stmt, i int) int64 {
	return int64(C.sqlite3_column_int64(s, C.int(i)))
}

// step runs one step and reports whether a row is available.
func (d *db) step(s *C.sqlite3_stmt) (bool, error) {
	switch rc := C.sqlite3_step(s); rc {
	case C.SQLITE_ROW:
		return true, nil
	case C.SQLITE_DONE:
		return false, nil
	default:
		return false, fmt.Errorf("step: %s (%d)", d.errmsg(), rc)
	}
}

// reset does not clear bindings, and that is checked rather than assumed: every statement in
// db.go either takes no parameters or binds all of its parameters before each use, so a cleared
// binding could never be observed. Clearing them was a second cgo crossing on every statement.
// A statement that ever reuses a binding across calls must not be added without revisiting this.
func reset(s *C.sqlite3_stmt) {
	C.sqlite3_reset(s)
}

func (d *db) changes() int { return int(C.sqlite3_changes(d.h)) }

func (d *db) pingOK() bool {
	defer reset(d.ping)
	ok, err := d.step(d.ping)
	return ok && err == nil
}
