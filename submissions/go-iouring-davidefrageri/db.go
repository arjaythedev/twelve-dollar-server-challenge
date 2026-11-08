package main

/*
#cgo CFLAGS: -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
#cgo CFLAGS: -DSQLITE_DQS=0 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_USE_ALLOCA
#cgo CFLAGS: -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE
#cgo CFLAGS: -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_TRACE
#cgo CFLAGS: -DSQLITE_OMIT_COMPLETE -DSQLITE_OMIT_GET_TABLE -DSQLITE_OMIT_TCL_VARIABLE -DSQLITE_OMIT_DECLTYPE
#cgo CFLAGS: -DSQLITE_OMIT_JSON -DSQLITE_TEMP_STORE=3 -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DHAVE_POSIX_FALLOCATE=1 -DNDEBUG
#cgo LDFLAGS: -lm -lpthread
#include <stdlib.h>
#include "shim.h"
*/
import "C"

import (
	"errors"
	"sync/atomic"
	"unsafe"
)

const (
	kFeed   = C.TD_FEED
	kPost   = C.TD_POST
	kLike   = C.TD_LIKE
	kInsert = C.TD_INSERT
	kHealth = C.TD_HEALTH

	spillFlag = C.TD_SPILL
)

// reqT mirrors td_req_t (shim.h) field for field, so Go fills the batch without conversions.
type reqT struct {
	kind, status uint8
	resv         uint16
	n            int32
	id           int64
	uid          int64
	body         unsafe.Pointer
	name         unsafe.Pointer
	nbody, nname int32
	out          unsafe.Pointer
	cap          int32
	resv2        int32
}

// The batch lives in C memory: its entries point at buffers outside the Go heap.
var reqs *[maxReqs]reqT

func reqAt(i int) *reqT { return &reqs[i] }

func dbOpen(path string) error {
	if unsafe.Sizeof(reqT{}) != C.sizeof_td_req_t {
		return errors.New("td_req_t layout mismatch")
	}
	reqs = (*[maxReqs]reqT)(C.calloc(maxReqs, C.sizeof_td_req_t))
	if reqs == nil {
		return errors.New("calloc")
	}
	p := C.CString(path)
	defer C.free(unsafe.Pointer(p))
	var buf [512]C.char
	if C.td_open(p, &buf[0], C.int(len(buf))) != 0 {
		return errors.New(C.GoString(&buf[0]))
	}
	return nil
}

// dbBatch runs the first n entries of reqs in one transaction; false if it was lost.
func dbBatch(n int) bool { return C.td_batch((*C.td_req_t)(unsafe.Pointer(reqs)), C.int(n)) == 0 }

func dbTick()          { C.td_tick() }
func writesHeld() bool { return C.td_writes_held() != 0 }

func dbStats(out []int64) { C.td_stats((*C.int64_t)(unsafe.Pointer(&out[0])), C.int(len(out))) }

func atomicLoad(p *uint32) uint32     { return atomic.LoadUint32(p) }
func atomicStore(p *uint32, v uint32) { atomic.StoreUint32(p, v) }
