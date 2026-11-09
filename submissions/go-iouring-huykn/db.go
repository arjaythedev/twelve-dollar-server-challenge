package main

/*
#cgo CFLAGS: -O3 -march=native -fno-plt -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
#cgo CFLAGS: -DSQLITE_DQS=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1
#cgo CFLAGS: -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_AUTOINIT
#cgo LDFLAGS: -lm -lpthread
#cgo nocallback hkn_read_handle
#cgo nocallback hkn_create_handle
#cgo nocallback hkn_vote_handle
#cgo nocallback hkn_finish_handle
#cgo nocallback hkn_probe_handle
#include <stdlib.h>
#include "bridge.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

// One owner (the locked event-loop thread) uses this handle. Records borrow its
// arena until the next read or insert; handlers serialize them immediately.
type database struct {
	handle C.hkn_handle
}

type postRecord struct {
	body, created, author []byte
	id, likes             int64
}

var storage database

func dbOpen(path string) error {
	filename := C.CString(path)
	defer C.free(unsafe.Pointer(filename))
	var message [256]C.char
	handle := C.hkn_open_handle(filename, &message[0], C.int(len(message)))
	if handle == 0 {
		return errors.New(C.GoString(&message[0]))
	}
	C.hkn_close_handle(storage.handle)
	storage = database{handle: handle}
	return nil
}

func dbClose() {
	C.hkn_close_handle(storage.handle)
	storage = database{}
}

// A view borrows the handle's owned arena until the next database operation.
// Convert only the row being serialized, without materializing another array.
type postRows struct {
	result *C.hkn_result
}

func (rows postRows) len() int { return int(rows.result.length) }

func (rows postRows) at(index int) *C.hkn_record { return &rows.result.items[index] }

func (rows postRows) text(span C.hkn_span) []byte {
	// Both C read paths bound spans to the owned arena before returning success.
	// Borrow the validated range directly rather than re-slicing the whole arena.
	start := unsafe.Add(unsafe.Pointer(&rows.result.bytes[0]), uintptr(span.at))
	return unsafe.Slice((*byte)(start), int(span.size))
}

func (rows postRows) record(index int) postRecord {
	r := rows.at(index)
	return postRecord{
		id: int64(r.key), likes: int64(r.count),
		body:    rows.text(r.content),
		created: rows.text(r.timestamp),
		author:  rows.text(r.username),
	}
}

func dbFeed() (postRows, bool) {
	result := C.hkn_read_handle(storage.handle, 0, 0)
	return postRows{result}, result != nil
}

func dbPost(id int64) (postRecord, int) {
	result := C.hkn_read_handle(storage.handle, 1, C.int64_t(id))
	if result == nil {
		return postRecord{}, -1
	}
	rows := postRows{result}
	if rows.len() == 0 {
		return postRecord{}, 0
	}
	return rows.record(0), 1
}

func dbInsertPost(user int64, body []byte) (int64, []byte, bool) {
	result := C.hkn_create_handle(storage.handle, C.int64_t(user),
		(*C.char)(unsafe.Pointer(unsafe.SliceData(body))), C.int(len(body)))
	if result == nil {
		return 0, nil, false
	}
	r := &result.items[0]
	created := unsafe.Slice((*byte)(unsafe.Pointer(&result.bytes[r.timestamp.at])), int(r.timestamp.size))
	return int64(r.key), created, true
}

// Preserve the application's outcome convention at the new adapter boundary.
func dbLike(user, post int64) int {
	switch C.hkn_vote_handle(storage.handle, C.int64_t(user), C.int64_t(post)) {
	case 201:
		return 2
	case 200:
		return 1
	case 404:
		return 0
	default:
		return -1
	}
}

func dbHealth() (bool, []byte) {
	if C.hkn_probe_handle(storage.handle) == 0 {
		return true, nil
	}
	return false, []byte(C.GoString(C.hkn_error_handle(storage.handle)))
}

func dbCommit() bool { return C.hkn_finish_handle(storage.handle) == 0 }
