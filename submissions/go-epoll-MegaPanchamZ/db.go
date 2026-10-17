package main

/*
#cgo CFLAGS: -O3 -march=native -I${SRCDIR}/.deps
#cgo LDFLAGS: -L${SRCDIR}/.deps -lsqlite3 -lm
#include "shim.h"
*/
import "C"

import (
	"errors"
	"unsafe"
)

const (
	opStatic     = C.OP_STATIC
	opHealth     = C.OP_HEALTH
	opFeed       = C.OP_FEED
	opGetPost    = C.OP_GET_POST
	opCreatePost = C.OP_CREATE_POST
	opLikePost   = C.OP_LIKE_POST

	maxBatchOps = 2048
	reqArenaCap = 2 << 20
	outArenaCap = 16 << 20
)

var pragmas = "PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON; PRAGMA mmap_size = 1073741824; PRAGMA cache_size = -2000; PRAGMA temp_store = MEMORY;"

type db struct {
	ctx    *C.db_ctx
	ops    []C.db_op
	reqBuf []byte
	outBuf []byte
	reqPos int32
	nOps   int
}

func openDB(path string) (*db, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	cprag := C.CString(pragmas)
	defer C.free(unsafe.Pointer(cprag))

	var cerr *C.char
	ctx := C.db_init(cpath, cprag, maxBatchOps, reqArenaCap, outArenaCap, &cerr)
	if ctx == nil {
		if cerr != nil {
			defer C.free(unsafe.Pointer(cerr))
			return nil, errors.New(C.GoString(cerr))
		}
		return nil, errors.New("db_init failed")
	}
	return &db{
		ctx:    ctx,
		ops:    unsafe.Slice(ctx.ops, maxBatchOps),
		reqBuf: unsafe.Slice((*byte)(unsafe.Pointer(ctx.req_buf)), reqArenaCap),
		outBuf: unsafe.Slice((*byte)(unsafe.Pointer(ctx.out_buf)), outArenaCap),
	}, nil
}

func (d *db) resetBatch() {
	d.nOps = 0
	d.reqPos = 0
}

func (d *db) allocText(b []byte) (int32, int32) {
	off := d.reqPos
	n := int32(len(b))
	copy(d.reqBuf[off:], b)
	d.reqPos += n
	return off, n
}

func (d *db) pushStatic(status uint16, body []byte, closeConn bool) int {
	idx := d.nOps
	off, n := d.allocText(body)
	var cc C.uint8_t
	if closeConn {
		cc = 1
	}
	d.ops[idx] = C.db_op{
		kind:       opStatic,
		close_conn: cc,
		status:     C.uint16_t(status),
		text_off:   C.int32_t(off),
		text_len:   C.int32_t(n),
	}
	d.nOps++
	return idx
}

func (d *db) pushSimple(kind uint8, arg1, arg2 int64, closeConn bool) int {
	idx := d.nOps
	var cc C.uint8_t
	if closeConn {
		cc = 1
	}
	d.ops[idx] = C.db_op{
		kind:       C.uint8_t(kind),
		close_conn: cc,
		arg1:       C.int64_t(arg1),
		arg2:       C.int64_t(arg2),
	}
	d.nOps++
	return idx
}

func (d *db) pushCreate(uid int64, body, user []byte, closeConn bool) int {
	idx := d.nOps
	toff, tlen := d.allocText(body)
	uoff, ulen := d.allocText(user)
	var cc C.uint8_t
	if closeConn {
		cc = 1
	}
	d.ops[idx] = C.db_op{
		kind:       opCreatePost,
		close_conn: cc,
		arg1:       C.int64_t(uid),
		text_off:   C.int32_t(toff),
		text_len:   C.int32_t(tlen),
		user_off:   C.int32_t(uoff),
		user_len:   C.int32_t(ulen),
	}
	d.nOps++
	return idx
}

func (d *db) execBatch(uptimeSec int64) {
	if d.nOps == 0 {
		return
	}
	C.db_exec_batch(d.ctx, C.int(d.nOps), C.int64_t(uptimeSec))
}

func (d *db) checkpointIfDue() {
	C.db_checkpoint_if_due(d.ctx)
}
