// Package sqlite is a thin cgo binding to the SQLite C API: open, prepare, bind,
// step, columns. No pooling, no database/sql, no schema knowledge. One Conn
// belongs to one goroutine (the amalgamation is built in multi-thread mode).
package sqlite

/*
// SQLITE_ENABLE_QPSG: never re-plan a statement from the values bound to it;
// without it SQLite 3.54 recompiles a query such as "WHERE p.id = ?" on every rebind.
#cgo CFLAGS: -O2 -DNDEBUG -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_USE_ALLOCA -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_ENABLE_QPSG
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "sqlite3.h"

// SQLITE_TRANSIENT is a function-pointer cast cgo cannot express.
static int bind_text_copy(sqlite3_stmt *s, int i, const char *p, int n) {
	return sqlite3_bind_text(s, i, p, n, SQLITE_TRANSIENT);
}
*/
import "C"

import (
	"fmt"
	"unsafe"
)

const (
	ErrBusy                 = 5
	ErrConstraintForeignKey = 787
)

// Error carries SQLite's extended result code.
type Error struct {
	Code int
	Msg  string
}

func (e *Error) Error() string { return fmt.Sprintf("sqlite: %s (%d)", e.Msg, e.Code) }

// Code returns the extended result code of err, or 0 for nil / non-SQLite errors.
func Code(err error) int {
	if e, ok := err.(*Error); ok {
		return e.Code
	}
	return 0
}

type Conn struct{ db *C.sqlite3 }

type Stmt struct {
	s    *C.sqlite3_stmt
	conn *Conn
}

// Open opens an existing database read-write. It never creates one.
func Open(path string) (*Conn, error) {
	return open(path, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_NOMUTEX)
}

// Create opens or creates a database; used by tests only.
func Create(path string) (*Conn, error) {
	return open(path, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_CREATE|C.SQLITE_OPEN_NOMUTEX)
}

func open(path string, flags C.int) (*Conn, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	var db *C.sqlite3
	rc := C.sqlite3_open_v2(cpath, &db, flags, nil)
	if rc != C.SQLITE_OK {
		msg := C.GoString(C.sqlite3_errmsg(db))
		C.sqlite3_close(db)
		return nil, &Error{Code: int(rc), Msg: msg}
	}
	C.sqlite3_extended_result_codes(db, 1)
	return &Conn{db: db}, nil
}

func (c *Conn) err(rc C.int) error {
	return &Error{Code: int(rc), Msg: C.GoString(C.sqlite3_errmsg(c.db))}
}

// Exec runs one or more SQL statements with no result rows (pragmas, DDL).
func (c *Conn) Exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var cerr *C.char
	rc := C.sqlite3_exec(c.db, csql, nil, nil, &cerr)
	if rc != C.SQLITE_OK {
		msg := C.GoString(cerr)
		C.sqlite3_free(unsafe.Pointer(cerr))
		return &Error{Code: int(rc), Msg: msg}
	}
	return nil
}

// Prepare compiles a persistent statement. The caller owns it until Finalize.
func (c *Conn) Prepare(sql string) (*Stmt, error) {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var s *C.sqlite3_stmt
	rc := C.sqlite3_prepare_v3(c.db, csql, C.int(len(sql)), C.SQLITE_PREPARE_PERSISTENT, &s, nil)
	if rc != C.SQLITE_OK {
		return nil, c.err(rc)
	}
	return &Stmt{s: s, conn: c}, nil
}

// Changes is the row count of the most recent INSERT/UPDATE/DELETE.
func (c *Conn) Changes() int { return int(C.sqlite3_changes(c.db)) }

func (c *Conn) Close() error {
	if rc := C.sqlite3_close(c.db); rc != C.SQLITE_OK {
		return c.err(rc)
	}
	c.db = nil
	return nil
}

func (s *Stmt) BindInt64(i int, v int64) error {
	if rc := C.sqlite3_bind_int64(s.s, C.int(i), C.sqlite3_int64(v)); rc != C.SQLITE_OK {
		return s.conn.err(rc)
	}
	return nil
}

// BindText copies v into SQLite's own memory.
func (s *Stmt) BindText(i int, v string) error {
	cv := C.CString(v)
	defer C.free(unsafe.Pointer(cv))
	if rc := C.bind_text_copy(s.s, C.int(i), cv, C.int(len(v))); rc != C.SQLITE_OK {
		return s.conn.err(rc)
	}
	return nil
}

// Step advances the statement: (true, nil) on a row, (false, nil) when done.
// On error the statement is reset so it can be bound and stepped again.
func (s *Stmt) Step() (bool, error) {
	switch rc := C.sqlite3_step(s.s); rc {
	case C.SQLITE_ROW:
		return true, nil
	case C.SQLITE_DONE:
		return false, nil
	default:
		err := s.conn.err(rc)
		C.sqlite3_reset(s.s)
		return false, err
	}
}

func (s *Stmt) ColumnInt64(i int) int64 { return int64(C.sqlite3_column_int64(s.s, C.int(i))) }

// ColumnRawText returns a view of column i's text in SQLite's memory. It is
// valid only until the next Step, Reset or Finalize; copy it if it must outlive them.
func (s *Stmt) ColumnRawText(i int) []byte {
	n := C.sqlite3_column_bytes(s.s, C.int(i))
	if n == 0 {
		return nil
	}
	p := C.sqlite3_column_text(s.s, C.int(i))
	return unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n))
}

func (s *Stmt) ColumnText(i int) string { return string(s.ColumnRawText(i)) }

// Reset makes the statement ready to run again and clears its bindings.
func (s *Stmt) Reset() error {
	rc := C.sqlite3_reset(s.s)
	C.sqlite3_clear_bindings(s.s)
	if rc != C.SQLITE_OK {
		return s.conn.err(rc)
	}
	return nil
}

// Reprepares is how many times SQLite has recompiled the statement from its
// SQL since it was prepared (schema changes, or bind-time re-planning).
func (s *Stmt) Reprepares() int {
	return int(C.sqlite3_stmt_status(s.s, C.SQLITE_STMTSTATUS_REPREPARE, 0))
}

func (s *Stmt) Finalize() {
	C.sqlite3_finalize(s.s)
	s.s = nil
}
