// Package sqlite is a minimal cgo binding to the system libsqlite3: open,
// prepare, bind, step, read columns. No database/sql in between, and a Conn
// is used by one goroutine at a time, so the library is opened without its
// own mutexes.
package sqlite

/*
#cgo LDFLAGS: -lsqlite3
#include <sqlite3.h>
#include <stdlib.h>

static int bind_text(sqlite3_stmt *s, int i, _GoString_ v) {
	return sqlite3_bind_text(s, i, _GoStringPtr(v), (int)_GoStringLen(v), SQLITE_TRANSIENT);
}
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// Error is a failed sqlite call: its result code and the connection's message.
type Error struct {
	Code int
	Msg  string
}

func (e *Error) Error() string { return fmt.Sprintf("sqlite %d: %s", e.Code, e.Msg) }

var ErrClosed = errors.New("sqlite: connection closed")

type Conn struct {
	db *C.sqlite3
}

// Open opens path read-write, or read-only when readOnly is set. The file must
// exist: the database is never created here.
func Open(path string, readOnly bool) (*Conn, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	flags := C.int(C.SQLITE_OPEN_READWRITE | C.SQLITE_OPEN_NOMUTEX)
	if readOnly {
		flags = C.SQLITE_OPEN_READONLY | C.SQLITE_OPEN_NOMUTEX
	}
	var db *C.sqlite3
	if rc := C.sqlite3_open_v2(cpath, &db, flags, nil); rc != C.SQLITE_OK {
		err := &Error{Code: int(rc), Msg: C.GoString(C.sqlite3_errstr(rc))}
		C.sqlite3_close_v2(db)
		return nil, err
	}
	c := &Conn{db: db}
	C.sqlite3_extended_result_codes(db, 1)
	return c, nil
}

func (c *Conn) err(rc C.int) error {
	return &Error{Code: int(rc), Msg: C.GoString(C.sqlite3_errmsg(c.db))}
}

// Exec runs one or more statements that return no rows.
func (c *Conn) Exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if rc := C.sqlite3_exec(c.db, csql, nil, nil, nil); rc != C.SQLITE_OK {
		return c.err(rc)
	}
	return nil
}

// BusyTimeout makes a locked database wait up to ms milliseconds.
func (c *Conn) BusyTimeout(ms int) { C.sqlite3_busy_timeout(c.db, C.int(ms)) }

// Changes is the number of rows the last INSERT, UPDATE or DELETE touched.
func (c *Conn) Changes() int64 { return int64(C.sqlite3_changes64(c.db)) }

func (c *Conn) Close() error {
	if c.db == nil {
		return ErrClosed
	}
	rc := C.sqlite3_close_v2(c.db)
	c.db = nil
	if rc != C.SQLITE_OK {
		return &Error{Code: int(rc), Msg: C.GoString(C.sqlite3_errstr(rc))}
	}
	return nil
}

// Stmt is a prepared statement, kept for the life of its connection.
type Stmt struct {
	c *Conn
	s *C.sqlite3_stmt
}

func (c *Conn) Prepare(sql string) (*Stmt, error) {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var s *C.sqlite3_stmt
	if rc := C.sqlite3_prepare_v3(c.db, csql, -1, C.SQLITE_PREPARE_PERSISTENT, &s, nil); rc != C.SQLITE_OK {
		return nil, c.err(rc)
	}
	return &Stmt{c: c, s: s}, nil
}

// BindInt64 binds v to the 1-based parameter i.
func (s *Stmt) BindInt64(i int, v int64) error {
	if rc := C.sqlite3_bind_int64(s.s, C.int(i), C.sqlite3_int64(v)); rc != C.SQLITE_OK {
		return s.c.err(rc)
	}
	return nil
}

// BindText binds v to the 1-based parameter i. sqlite copies it.
func (s *Stmt) BindText(i int, v string) error {
	if rc := C.bind_text(s.s, C.int(i), v); rc != C.SQLITE_OK {
		return s.c.err(rc)
	}
	return nil
}

// Step advances to the next row: true with a row to read, false when done.
func (s *Stmt) Step() (bool, error) {
	switch rc := C.sqlite3_step(s.s); rc {
	case C.SQLITE_ROW:
		return true, nil
	case C.SQLITE_DONE:
		return false, nil
	default:
		return false, s.c.err(rc)
	}
}

// Int64 reads the 0-based column i of the current row.
func (s *Stmt) Int64(i int) int64 { return int64(C.sqlite3_column_int64(s.s, C.int(i))) }

// Text reads the 0-based column i of the current row into a Go string.
func (s *Stmt) Text(i int) string {
	p := C.sqlite3_column_text(s.s, C.int(i))
	n := C.sqlite3_column_bytes(s.s, C.int(i))
	if p == nil || n == 0 {
		return ""
	}
	return C.GoStringN((*C.char)(unsafe.Pointer(p)), n)
}

// Reset readies the statement for its next run and clears its bindings. A
// statement must be reset after every use, or a read holds its snapshot and
// a write its lock.
func (s *Stmt) Reset() {
	C.sqlite3_reset(s.s)
	C.sqlite3_clear_bindings(s.s)
}

func (s *Stmt) Close() error {
	if rc := C.sqlite3_finalize(s.s); rc != C.SQLITE_OK {
		return s.c.err(rc)
	}
	return nil
}
