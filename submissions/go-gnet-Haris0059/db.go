package main

/*
#cgo CFLAGS: -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DHAVE_USLEEP=1
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include <string.h>
#include "sqlite/sqlite3.h"

// Each helper is exactly one cgo call: bind, step, copy the result out, reset.

// q_text: optional int64 param 1, copies column 0 of the first row into buf.
// Returns SQLITE_ROW (found), SQLITE_DONE (no row) or an error; *n is the full length.
static int q_text(sqlite3_stmt *s, sqlite3_int64 arg, char *buf, int cap, int *n) {
	if (arg > 0) sqlite3_bind_int64(s, 1, arg);
	int rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(s, 0);
		int len = sqlite3_column_bytes(s, 0);
		if (len <= cap) memcpy(buf, t, len);
		*n = len;
	}
	sqlite3_reset(s);
	return rc;
}

// q_insert: INSERT ... RETURNING id, created_at. Steps to SQLITE_DONE so the row is committed.
static int q_insert(sqlite3_stmt *s, sqlite3_int64 uid, const char *body, int blen,
                    sqlite3_int64 *id, char *created, int cap, int *n) {
	sqlite3_bind_int64(s, 1, uid);
	sqlite3_bind_text(s, 2, body, blen, SQLITE_TRANSIENT);
	int rc = sqlite3_step(s);
	if (rc == SQLITE_ROW) {
		*id = sqlite3_column_int64(s, 0);
		const unsigned char *t = sqlite3_column_text(s, 1);
		int len = sqlite3_column_bytes(s, 1);
		if (len > cap) len = cap;
		memcpy(created, t, len);
		*n = len;
		rc = sqlite3_step(s);
	}
	sqlite3_reset(s);
	return rc;
}

// q_like: binds (uid, pid, pid), returns the step result; *changes is rows inserted.
static int q_like(sqlite3_stmt *s, sqlite3_int64 uid, sqlite3_int64 pid, int *changes) {
	sqlite3_bind_int64(s, 1, uid);
	sqlite3_bind_int64(s, 2, pid);
	sqlite3_bind_int64(s, 3, pid);
	int rc = sqlite3_step(s);
	*changes = sqlite3_changes(sqlite3_db_handle(s));
	sqlite3_reset(s);
	return rc;
}

static int prep(sqlite3 *db, const char *sql, sqlite3_stmt **s) {
	return sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, s, 0);
}
*/
import "C"

import (
	"errors"
	"log"
	"slices"
	"sync"
	"unsafe"
)

var (
	// One connection, serialized: on 1 vCPU parallel SQLite calls buy nothing.
	mu                                           sync.Mutex
	db                                           *C.sqlite3
	feedS, postS, existsS, insertS, likeS, pingS *C.sqlite3_stmt
)

func dbErr() error { return errors.New(C.GoString(C.sqlite3_errmsg(db))) }

func dbOpen(path string) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	// NOMUTEX: our Go mutex already serializes every call on this connection.
	if C.sqlite3_open_v2(cpath, &db, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_NOMUTEX, nil) != C.SQLITE_OK {
		log.Fatal("open: ", dbErr())
	}
	cp := C.CString(pragmas)
	defer C.free(unsafe.Pointer(cp))
	if C.sqlite3_exec(db, cp, nil, nil, nil) != C.SQLITE_OK {
		log.Fatal("pragmas: ", dbErr())
	}
	for _, p := range []struct {
		s   **C.sqlite3_stmt
		sql string
	}{{&feedS, feedSQL}, {&postS, postSQL}, {&existsS, existsSQL}, {&insertS, insertSQL}, {&likeS, likeSQL}, {&pingS, "SELECT 1"}} {
		cs := C.CString(p.sql)
		rc := C.prep(db, cs, p.s)
		C.free(unsafe.Pointer(cs))
		if rc != C.SQLITE_OK {
			log.Fatal("prepare: ", dbErr())
		}
	}
}

// text runs a one-column query straight into out's spare capacity.
func text(s *C.sqlite3_stmt, arg int64, out []byte) ([]byte, bool, error) {
	mu.Lock()
	defer mu.Unlock()
	for {
		if cap(out)-len(out) < 1024 {
			out = slices.Grow(out, 16<<10)
		}
		spare := out[len(out):cap(out)]
		var n C.int
		rc := C.q_text(s, C.sqlite3_int64(arg), (*C.char)(unsafe.Pointer(&spare[0])), C.int(len(spare)), &n)
		switch {
		case rc == C.SQLITE_DONE:
			return out, false, nil
		case rc != C.SQLITE_ROW:
			return out, false, dbErr()
		case int(n) > len(spare): // didn't fit: grow and rerun (rare)
			out = slices.Grow(out, int(n))
			continue
		}
		return out[:len(out)+int(n)], true, nil
	}
}

func dbFeed(out []byte) ([]byte, error) {
	b, _, err := text(feedS, 0, out)
	return b, err
}

func dbPost(out []byte, id int64) ([]byte, bool, error) { return text(postS, id, out) }

func dbPing() error {
	_, _, err := text(pingS, 0, nil)
	return err
}

func dbInsert(uid int64, body string) (int64, string, error) {
	mu.Lock()
	defer mu.Unlock()
	var id C.sqlite3_int64
	var created [64]byte
	var n C.int
	rc := C.q_insert(insertS, C.sqlite3_int64(uid), (*C.char)(unsafe.Pointer(unsafe.StringData(body))), C.int(len(body)),
		&id, (*C.char)(unsafe.Pointer(&created[0])), C.int(len(created)), &n)
	if rc != C.SQLITE_DONE {
		return 0, "", dbErr()
	}
	return int64(id), string(created[:n]), nil
}

func dbLike(uid, id int64) (inserted, exists bool, err error) {
	mu.Lock()
	var changes C.int
	rc := C.q_like(likeS, C.sqlite3_int64(uid), C.sqlite3_int64(id), &changes)
	mu.Unlock()
	if rc != C.SQLITE_DONE {
		return false, false, dbErr()
	}
	if changes == 1 {
		return true, true, nil
	}
	_, found, err := text(existsS, id, nil)
	return false, found, err
}
