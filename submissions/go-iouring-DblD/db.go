//go:build linux

// The Go side of the SQLite shim (see db.h). cgo compiles db.c, which includes sqlite/sqlite3.c, which build.sh
// downloads next to this file, into the binary.
package main

/*
#cgo CFLAGS: -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DQS=0 -DHAVE_USLEEP=1
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "db.h"
*/
import "C"

import (
	"encoding/binary"
	"errors"
	"unsafe"
)

func dbOpen(path string) error {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.db_open(cpath) != 0 {
		return dbErr()
	}
	return nil
}

func dbErr() error { return errors.New(C.GoString(C.db_error())) }

func dbBegin() bool  { return C.db_begin() == 0 }
func dbCommit() bool { return C.db_commit() == 0 }

func dbUrgent(on bool) {
	if on {
		C.db_urgent(1)
	} else {
		C.db_urgent(0)
	}
}

func dbBackfills() int { return int(C.db_backfills()) }

func dbHoldWanted() bool { return C.db_hold_wanted() != 0 }

func dbReadsDirect() bool { return C.db_reads_direct() != 0 }

func dbStats() (walPages, walMax, checkpoints int) {
	var a, b, c C.int
	C.db_stats(&a, &b, &c)
	return int(a), int(b), int(c)
}

func dbHealth() bool { return C.db_health() == 1 }

// dbFeed and dbPost fill buf with packed rows and return how many bytes they wrote.
func dbFeed(buf []byte) int {
	return int(C.db_feed((*C.uint8_t)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
}

func dbPost(id int64, buf []byte) int {
	return int(C.db_post(C.int64_t(id), (*C.uint8_t)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
}

func dbPostExists(id int64) int { return int(C.db_post_exists(C.int64_t(id))) }

func dbLike(userID, postID int64) int { return int(C.db_like(C.int64_t(userID), C.int64_t(postID))) }

func dbCreate(userID int64, body []byte) (id int64, createdAt [24]byte, ok bool) {
	var cid C.int64_t
	rc := C.db_create(C.int64_t(userID), (*C.char)(unsafe.Pointer(&body[0])), C.int(len(body)), &cid, (*C.char)(unsafe.Pointer(&createdAt[0])))
	return int64(cid), createdAt, rc == 0
}

// row is one post as the shim packed it. The byte slices point into the shared buffer and
// are only valid until the next database call.
type row struct {
	id, likes             int64
	body, created, author []byte
}

func nextRow(b []byte) (row, []byte) {
	r := row{id: int64(binary.LittleEndian.Uint64(b)), likes: int64(binary.LittleEndian.Uint64(b[8:]))}
	bl, cl, al := int(binary.LittleEndian.Uint16(b[16:])), int(binary.LittleEndian.Uint16(b[18:])), int(binary.LittleEndian.Uint16(b[20:]))
	b = b[22:]
	r.body, r.created, r.author = b[:bl], b[bl:bl+cl], b[bl+cl:bl+cl+al]
	return r, b[bl+cl+al:]
}
