package main

/*
// SQLite is compiled by build.sh into sqlite/libsqlite3.a (compile options live there too).
#cgo LDFLAGS: ${SRCDIR}/sqlite/libsqlite3.a -lm
#include <stdlib.h>
#include "shim.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// query identifies one of the prepared statements the connection holds.
type query uint8

const (
	qFeed query = iota
	qPost
	qCreate
	qLike
	qExists
	qPing
	qBegin
	qCommit
	qRollback
	numQueries
)

var querySQL = [numQueries]string{
	qFeed: sqlFeed, qPost: sqlPost, qCreate: sqlCreate, qLike: sqlLike, qExists: sqlExists, qPing: sqlPing,
	qBegin: "BEGIN IMMEDIATE", qCommit: "COMMIT", qRollback: "ROLLBACK",
}

var errDB = errors.New("sqlite error")

// conn is the one SQLite connection (SQLITE_THREADSAFE=0): only the DB goroutine may touch it.
// Every query is a single cgo call into shim.c.
type conn struct {
	db    *C.sqlite3
	stmts [numQueries]*C.sqlite3_stmt
}

func openConn(path string) (*conn, error) {
	if rc := C.sqlite3_initialize(); rc != C.SQLITE_OK {
		return nil, fmt.Errorf("sqlite3_initialize: %d", rc)
	}
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	c := &conn{}
	if rc := C.sqlite3_open_v2(cpath, &c.db, C.SQLITE_OPEN_READWRITE, nil); rc != C.SQLITE_OK {
		return nil, fmt.Errorf("open %s: %s", path, C.GoString(C.sqlite3_errstr(rc)))
	}
	for _, p := range pragmas {
		if err := c.execSQL(p); err != nil {
			return nil, err
		}
	}
	for q, sql := range querySQL {
		csql := C.CString(sql)
		rc := C.sqlite3_prepare_v3(c.db, csql, -1, C.SQLITE_PREPARE_PERSISTENT, &c.stmts[q], nil)
		C.free(unsafe.Pointer(csql))
		if rc != C.SQLITE_OK {
			return nil, fmt.Errorf("prepare %q: %s", sql, c.errmsg())
		}
	}
	C.shim_install_wal_hook(c.db)
	return c, nil
}

func (c *conn) errmsg() string { return C.GoString(C.sqlite3_errmsg(c.db)) }

func (c *conn) execSQL(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if rc := C.sqlite3_exec(c.db, csql, nil, nil, nil); rc != C.SQLITE_OK {
		return fmt.Errorf("%s: %s", sql, c.errmsg())
	}
	return nil
}

func cptr(b []byte) *C.char {
	if len(b) == 0 {
		return nil
	}
	return (*C.char)(unsafe.Pointer(unsafe.SliceData(b)))
}

// run executes q with up to two integer and two text parameters (a nil text is not bound) and
// appends the single text column of the first row to out. rows is 0 when no row came back.
func (c *conn) run(q query, i1, i2 int64, t1, t2 []byte, out []byte) (_ []byte, rows int, err error) {
	n1, n2 := -1, -1
	if t1 != nil {
		n1 = len(t1)
	}
	if t2 != nil {
		n2 = len(t2)
	}
	for {
		room := cap(out) - len(out)
		n := int(C.shim_query(c.stmts[q], C.sqlite3_int64(i1), C.sqlite3_int64(i2), cptr(t1), C.int(n1), cptr(t2), C.int(n2),
			(*C.uchar)(unsafe.Pointer(unsafe.SliceData(out[len(out):cap(out)]))), C.int(room)))
		switch {
		case n < 0:
			return out, 0, fmt.Errorf("%w: %s", errDB, c.errmsg())
		case n == 0:
			return out, 0, nil
		case n <= room:
			return out[:len(out)+n], 1, nil
		}
		out = grow(out, n)[:len(out)]
	}
}

// exec runs a statement without result columns and returns the number of rows changed.
func (c *conn) exec(q query, i1, i2 int64) (int, error) {
	if q >= qBegin { // transaction control: no parameters, no row count
		rc := C.sqlite3_step(c.stmts[q])
		C.sqlite3_reset(c.stmts[q])
		if rc != C.SQLITE_DONE {
			return 0, fmt.Errorf("%w: %s", errDB, c.errmsg())
		}
		return 0, nil
	}
	n := int(C.shim_exec(c.stmts[q], C.sqlite3_int64(i1), C.sqlite3_int64(i2)))
	if n < 0 {
		return 0, fmt.Errorf("%w: %s", errDB, c.errmsg())
	}
	return n, nil
}

// walFrames is the WAL size after the last commit, in frames; checkpoint truncates the WAL.
func (c *conn) walFrames() int { return int(C.shim_wal_frames()) }
func (c *conn) checkpoint()    { C.shim_checkpoint(c.db) }
