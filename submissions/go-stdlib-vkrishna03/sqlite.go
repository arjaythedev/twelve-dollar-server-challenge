package main

/*
// SQLite 3.53.4, compiled into the binary from the amalgamation that build.sh downloads into
// sqlite/ (sqlite_amalgamation.c includes it). Options from SQLite's "recommended compile-time
// options": one thread uses SQLite (THREADSAFE=0: no mutexes), no memory statistics, and
// features this server never uses left out.
#cgo CFLAGS: -I${SRCDIR}/sqlite -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0
#cgo CFLAGS: -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
#cgo CFLAGS: -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED
#cgo CFLAGS: -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_LOAD_EXTENSION
#cgo CFLAGS: -DSQLITE_OMIT_AUTOINIT -DSQLITE_USE_ALLOCA -DSQLITE_STRICT_SUBTYPE=1
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "sqlite3.h"

// One result row, filled by a single step_row call, so a row costs one cgo call instead of one
// per column.
typedef struct {
	sqlite3_int64 i[8];
	const unsigned char *t[8];
	int n[8];
} row;

static int setup(void) {
	return sqlite3_initialize(); // required with SQLITE_OMIT_AUTOINIT
}

// Steps s once. On SQLITE_ROW, column c goes to r->t/r->n if bit c of text is set, else to r->i.
static int step_row(sqlite3_stmt *s, int ncol, unsigned text, row *r) {
	int rc = sqlite3_step(s);
	if (rc != SQLITE_ROW) return rc;
	for (int c = 0; c < ncol; c++) {
		if (text & (1u << c)) {
			r->t[c] = sqlite3_column_text(s, c);
			r->n[c] = sqlite3_column_bytes(s, c);
		} else {
			r->i[c] = sqlite3_column_int64(s, c);
		}
	}
	return rc;
}

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

func sqliteSetup() error {
	if rc := C.setup(); rc != C.SQLITE_OK {
		return fmt.Errorf("sqlite3_initialize: %s", C.GoString(C.sqlite3_errstr(rc)))
	}
	return nil
}

type db struct{ p *C.sqlite3 }

func openDB(path string) (*db, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	var p *C.sqlite3
	if rc := C.sqlite3_open_v2(cpath, &p, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_NOMUTEX, nil); rc != C.SQLITE_OK {
		C.sqlite3_close(p)
		return nil, fmt.Errorf("open %s: %s", path, C.GoString(C.sqlite3_errstr(rc)))
	}
	return &db{p}, nil
}

func (d *db) err() error { return errors.New(C.GoString(C.sqlite3_errmsg(d.p))) }

func (d *db) exec(sql string) error {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	if C.sqlite3_exec(d.p, csql, nil, nil, nil) != C.SQLITE_OK {
		return fmt.Errorf("%s: %w", sql, d.err())
	}
	return nil
}

// changes is the number of rows the last INSERT/UPDATE/DELETE on this connection changed.
func (d *db) changes() int64 { return int64(C.sqlite3_changes64(d.p)) }

// stmt is a prepared statement that is reused for the life of the connection. That caches the
// query plan, not data (rule 5).
type stmt struct {
	db       *db
	p        *C.sqlite3_stmt
	r        *C.row // C memory, so passing it to C needs no cgo pointer checks
	ncol     C.int
	textCols C.uint // bit c set: column c is read as text
}

func (d *db) prepare(sql string, textCols ...int) (*stmt, error) {
	csql := C.CString(sql)
	defer C.free(unsafe.Pointer(csql))
	var p *C.sqlite3_stmt
	if C.sqlite3_prepare_v3(d.p, csql, -1, C.SQLITE_PREPARE_PERSISTENT, &p, nil) != C.SQLITE_OK {
		return nil, fmt.Errorf("prepare %q: %w", sql, d.err())
	}
	s := &stmt{db: d, p: p, r: (*C.row)(C.calloc(1, C.sizeof_row)), ncol: C.sqlite3_column_count(p)}
	if s.ncol > 8 {
		return nil, fmt.Errorf("prepare %q: more than 8 columns", sql)
	}
	for _, c := range textCols {
		s.textCols |= 1 << c
	}
	return s, nil
}

// step moves to the next row: true if there is one, false when the statement is done.
func (s *stmt) step() (bool, error) {
	switch C.step_row(s.p, s.ncol, s.textCols, s.r) {
	case C.SQLITE_ROW:
		return true, nil
	case C.SQLITE_DONE:
		return false, nil
	default:
		return false, s.db.err()
	}
}

// reset ends the statement's read or write transaction; call it as soon as the rows are used.
func (s *stmt) reset() { C.sqlite3_reset(s.p) }

func (s *stmt) bindInt(i int, v int64) { C.sqlite3_bind_int64(s.p, C.int(i), C.sqlite3_int64(v)) }

func (s *stmt) bindText(i int, v string) { C.bind_text(s.p, C.int(i), v) }

func (s *stmt) int64(c int) int64 { return int64(s.r.i[c]) }

// text points into SQLite's memory without copying: it is only valid until the next step or reset.
func (s *stmt) text(c int) string {
	if s.r.t[c] == nil {
		return ""
	}
	return unsafe.String((*byte)(unsafe.Pointer(s.r.t[c])), int(s.r.n[c]))
}
