package main

/*
#cgo CFLAGS: -O2 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
#cgo CFLAGS: -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0
#cgo CFLAGS: -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
#cgo CFLAGS: -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_USE_ALLOCA
#cgo LDFLAGS: -lm -lpthread
#include <stdlib.h>
#include "fastdb.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

// Results of the C functions (see fastdb.h).
const (
	dbErr     = -1
	dbNoSpace = -2
)

func ptr(b []byte) *C.char { return (*C.char)(unsafe.Pointer(unsafe.SliceData(b))) }

func dbOpen(path string) error {
	var errbuf [256]byte
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.fdb_open(cpath, ptr(errbuf[:]), C.int(len(errbuf))) != 0 {
		return errors.New(cstr(errbuf[:]))
	}
	return nil
}

// dbPing runs SELECT 1 and returns SQLite's error message if it fails.
func dbPing() (string, bool) {
	var errbuf [256]byte
	if C.fdb_ping(ptr(errbuf[:]), C.int(len(errbuf))) != 0 {
		return cstr(errbuf[:]), false
	}
	return "", true
}

// The render functions write into out (its full length is usable) and return the byte count.
func dbFeed(out []byte) int { return int(C.fdb_feed(ptr(out), C.int(len(out)))) }

func dbPost(id int64, out []byte) int {
	return int(C.fdb_post(C.int64_t(id), ptr(out), C.int(len(out))))
}

func dbBegin() bool    { return C.fdb_begin() == 0 }
func dbCommit() bool   { return C.fdb_commit() == 0 }
func dbRollback() bool { return C.fdb_rollback() == 0 }

func dbLike(userID, postID int64) int { return int(C.fdb_like(C.int64_t(userID), C.int64_t(postID))) }

func dbCreate(userID int64, body, author string, out []byte) int {
	return int(C.fdb_create(C.int64_t(userID),
		(*C.char)(unsafe.Pointer(unsafe.StringData(body))), C.int(len(body)),
		(*C.char)(unsafe.Pointer(unsafe.StringData(author))), C.int(len(author)),
		ptr(out), C.int(len(out))))
}

func dbWALFrames() int { return int(C.fdb_wal_frames()) }

func dbCheckpointOpen(path string) error {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	if C.fdb_ckpt_open(cpath) != 0 {
		return errors.New("cannot open the checkpoint connection")
	}
	return nil
}

func dbCheckpoint() { C.fdb_checkpoint() }

func cstr(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}
