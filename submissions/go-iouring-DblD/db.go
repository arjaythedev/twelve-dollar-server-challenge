//go:build linux

// The Go side of the SQLite shim (see db.h). build.sh compiles db.c, which includes sqlite/sqlite3.c, into
// db.syso; the go tool links every .syso file it finds next to the Go files.
package main

/*
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "db.h"
*/
import "C"

import (
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

func dbPageMemory() (blockKB, hugeKB int) {
	var a, b C.long
	C.db_page_memory(&a, &b)
	return int(a), int(b)
}

func dbTextMemory() (movedKB, hugeKB int, left string) {
	var a, b C.long
	why := C.db_text_memory(&a, &b)
	return int(a), int(b), C.GoString(why)
}

func dbHealth() bool { return C.db_health() == 1 }

// dbFeed and dbPost write the JSON body into buf, which is the free part of a response buffer
// (memory outside the Go heap), and return its length; see db.h for the other values.
func dbFeed(buf []byte) int {
	return int(C.db_feed((*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(buf))), C.int(len(buf))))
}

func dbPost(id int64, buf []byte) int {
	return int(C.db_post(C.int64_t(id), (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(buf))), C.int(len(buf))))
}

// dbJSONString is the C side's string escaper, here so that json_test.go can compare it with
// appendString (cgo cannot be used from a test file).
func dbJSONString(dst, s []byte) int {
	return int(C.db_json_string((*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(s))), C.int(len(s)), (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(dst))), C.int(len(dst))))
}

// dbJSONInt is the C side's integer writer, for the same reason.
func dbJSONInt(dst []byte, v int64) int {
	return int(C.db_json_int(C.int64_t(v), (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(dst))), C.int(len(dst))))
}

func dbPostExists(id int64) int { return int(C.db_post_exists(C.int64_t(id))) }

func dbLike(userID, postID int64) int { return int(C.db_like(C.int64_t(userID), C.int64_t(postID))) }

func dbCreate(userID int64, body []byte) (id int64, createdAt [24]byte, ok bool) {
	var cid C.int64_t
	rc := C.db_create(C.int64_t(userID), (*C.char)(unsafe.Pointer(&body[0])), C.int(len(body)), &cid, (*C.char)(unsafe.Pointer(&createdAt[0])))
	return int64(cid), createdAt, rc == 0
}

// row is one post, for appendPost.
type row struct {
	id, likes             int64
	body, created, author []byte
}
