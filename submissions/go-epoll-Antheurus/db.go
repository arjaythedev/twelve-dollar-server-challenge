package main

/*
#cgo CFLAGS: -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
#cgo CFLAGS: -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE
#cgo CFLAGS: -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
#cgo CFLAGS: -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_USE_ALLOCA
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "shim.h"
*/
import "C"

import (
	"encoding/binary"
	"errors"
	"fmt"
	"unsafe"
)

const (
	sqlFeed = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20`
	// Same columns as sqlFeed, but scalar subqueries: for a single row the JOIN carries a fixed
	// per-execution cost that the subqueries do not, while for twenty rows the JOIN is cheaper.
	sqlPost = `SELECT p.id, p.body, p.created_at,
       (SELECT username FROM users WHERE id = p.user_id),
       (SELECT count(*) FROM likes WHERE post_id = p.id)
  FROM posts p WHERE p.id = ?1`
	sqlCreate = `INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at`
	// The EXISTS guard keeps a like on a missing post from inserting a dangling row (foreign keys
	// are off); a zero change count is then told apart by sqlExists, so the common case is one query.
	sqlLike   = `INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING`
	sqlExists = `SELECT 1 FROM posts WHERE id = ?1`
	sqlPing   = `SELECT 1`
	sqlBegin  = `BEGIN IMMEDIATE`
	sqlCommit = `COMMIT`
	sqlRoll   = `ROLLBACK`
)

const outCap = 128 << 10

type stmt struct {
	s    *C.sqlite3_stmt
	spec *C.char
}

// db is one SQLite connection driven from a single goroutine. Every buffer handed to C lives in
// C memory, so no call pays for cgo pointer checks.
type db struct {
	conn *C.sqlite3
	io   *C.tq_io
	out  []byte
	text []byte

	feed, post, create, like, exists, ping, begin, commit, rollback stmt
}

var errFull = errors.New("result buffer too small")

func openDB(path string, pragmas []string) (*db, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	d := &db{}
	flags := C.int(C.SQLITE_OPEN_READWRITE | C.SQLITE_OPEN_NOMUTEX)
	if rc := C.sqlite3_open_v2(cpath, &d.conn, flags, nil); rc != C.SQLITE_OK {
		return nil, fmt.Errorf("open %s: %s", path, C.GoString(C.sqlite3_errstr(rc)))
	}
	d.io = (*C.tq_io)(C.calloc(1, C.size_t(unsafe.Sizeof(C.tq_io{}))))
	C.tq_set_busy(d.conn, d.io)
	for _, p := range pragmas {
		if err := d.exec(p); err != nil {
			return nil, err
		}
	}
	d.out = unsafe.Slice((*byte)(C.malloc(outCap)), outCap)
	d.text = unsafe.Slice((*byte)(C.malloc(outCap)), outCap)[:0]
	return d, nil
}

func (d *db) exec(sql string) error {
	c := C.CString(sql)
	defer C.free(unsafe.Pointer(c))
	if rc := C.sqlite3_exec(d.conn, c, nil, nil, nil); rc != C.SQLITE_OK {
		return fmt.Errorf("%s: %s", sql, d.errmsg())
	}
	return nil
}

func (d *db) prepare(sql, spec string) (stmt, error) {
	c := C.CString(sql)
	defer C.free(unsafe.Pointer(c))
	var s *C.sqlite3_stmt
	if rc := C.sqlite3_prepare_v3(d.conn, c, -1, C.SQLITE_PREPARE_PERSISTENT, &s, nil); rc != C.SQLITE_OK {
		return stmt{}, fmt.Errorf("prepare %q: %s", sql, d.errmsg())
	}
	return stmt{s: s, spec: C.CString(spec)}, nil
}

func (d *db) prepareAll() error {
	for _, p := range []struct {
		st        *stmt
		sql, spec string
	}{
		{&d.feed, sqlFeed, "ittti"},
		{&d.post, sqlPost, "ittti"},
		{&d.create, sqlCreate, "it"},
		{&d.like, sqlLike, ""},
		{&d.exists, sqlExists, "i"},
		{&d.ping, sqlPing, "i"},
		{&d.begin, sqlBegin, ""},
		{&d.commit, sqlCommit, ""},
		{&d.rollback, sqlRoll, ""},
	} {
		st, err := d.prepare(p.sql, p.spec)
		if err != nil {
			return err
		}
		*p.st = st
	}
	return nil
}

func (d *db) errmsg() string { return C.GoString(C.sqlite3_errmsg(d.conn)) }

// run executes st with the given integer parameters and, when text is non-nil, one text
// parameter after them. It returns the row count; the packed rows are in d.out[:d.used()].
func (d *db) run(st stmt, text []byte, ints ...int64) (int, error) {
	for i, v := range ints {
		d.io.ints[i] = C.longlong(v)
	}
	var tp *C.char
	tlen := C.int(-1)
	if text != nil {
		tlen = C.int(len(text))
		if len(text) > 0 {
			tp = (*C.char)(unsafe.Pointer(unsafe.SliceData(text)))
		}
	}
	rows := C.tq_run(st.s, d.io, C.int(len(ints)), tp, tlen, st.spec,
		(*C.uchar)(unsafe.Pointer(unsafe.SliceData(d.out))), outCap)
	if rows == C.TQ_FULL {
		return 0, errFull
	}
	if rows < 0 {
		return 0, sqliteError{d.errmsg()}
	}
	return int(rows), nil
}

// tryBegin opens the write transaction without waiting for the write lock; false means it is
// held (by the checkpointer or the other worker) and the caller should try again shortly.
func (d *db) tryBegin() (bool, error) {
	switch rc := C.tq_try_begin(d.begin.s, d.io); rc {
	case C.SQLITE_DONE:
		return true, nil
	case C.SQLITE_BUSY:
		return false, nil
	default:
		return false, sqliteError{d.errmsg()}
	}
}

func (d *db) used() int    { return int(d.io.used) }
func (d *db) changes() int { return int(d.io.changes) }

type sqliteError struct{ msg string }

func (e sqliteError) Error() string { return e.msg }

func (d *db) checkpoint() { C.tq_checkpoint(d.conn) }

// rowReader walks the packed rows tq_run wrote.
type rowReader struct {
	b   []byte
	pos int
}

func (r *rowReader) int() int64 {
	v := int64(binary.NativeEndian.Uint64(r.b[r.pos:]))
	r.pos += 8
	return v
}

func (r *rowReader) text() []byte {
	n := int(binary.NativeEndian.Uint32(r.b[r.pos:]))
	t := r.b[r.pos+4 : r.pos+4+n]
	r.pos += 4 + n
	return t
}
