package main

/*
#cgo CFLAGS: -O3 -march=native -fno-plt -w
#cgo CFLAGS: -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
#cgo CFLAGS: -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
#cgo CFLAGS: -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
#cgo CFLAGS: -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
#cgo CFLAGS: -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE
#cgo CFLAGS: -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
#cgo LDFLAGS: -lm -lpthread
#include "shim.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

// The rows and text of the last read, in C memory (td_out in shim.c). Valid until the next call.
var (
	out     = &C.td_out
	outText = unsafe.Slice((*byte)(unsafe.Pointer(&C.td_out.text[0])), C.TD_TEXT)
)

type row = C.td_row_t

func dbOpen(path string) error {
	p := C.CString(path)
	var buf [256]C.char
	if C.td_open(p, &buf[0], C.int(len(buf))) != 0 {
		return errors.New(C.GoString(&buf[0]))
	}
	return nil
}

func text(off, n C.uint32_t) []byte { return outText[off : off+n] }

// dbFeed returns the feed's rows (nil, false on error).
func dbFeed() ([]row, bool) {
	n := cFeed()
	if n < 0 {
		return nil, false
	}
	return out.rows[:n], true
}

// render writes the last read's JSON body into b's spare capacity (td_render) and returns its
// length, or -1 if it might not fit (the caller then builds it in Go).
func render(kind int, b []byte) int {
	free := cap(b) - len(b)
	if free < 512 {
		return -1
	}
	p := unsafe.Pointer(unsafe.SliceData(b[len(b):cap(b)]))
	return cRender(kind, p, free)
}

// dbPost: 1 found (out.rows[0]), 0 missing, -1 error.
func dbPost(id int64) int { return cPost(id) }

// dbLike: 2 new, 1 repeat, 0 no such post, -1 error.
func dbLike(uid, pid int64) int { return cLike(uid, pid) }

// dbInsertPost returns the new id and created_at (in C memory).
func dbInsertPost(uid int64, body []byte) (int64, []byte, bool) {
	if cInsert(uid, unsafe.Pointer(&body[0]), len(body)) != 0 {
		return 0, nil, false
	}
	r := &out.rows[0]
	return int64(r.id), text(r.created_off, r.created_len), true
}

func dbHealth() (bool, []byte) {
	if cHealth() != 0 {
		return false, outText[:out.ntext]
	}
	return true, nil
}

func dbCommit() bool   { return cCommit() == 0 }
func dbTick()          { cTick() }
func writesHeld() bool { return cHeld() != 0 }
