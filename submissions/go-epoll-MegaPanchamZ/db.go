package main

/*
#cgo CFLAGS: -O3 -march=native -I${SRCDIR}/.deps
#cgo LDFLAGS: ${SRCDIR}/.deps/libsqlite3.a -lm
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

var pragmas = "PRAGMA locking_mode = EXCLUSIVE; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = OFF; PRAGMA ignore_check_constraints = ON; PRAGMA mmap_size = 0; PRAGMA cache_size = -262144; PRAGMA temp_store = MEMORY; PRAGMA wal_autocheckpoint = 0;"

type db struct {
	ctx     *C.db_ctx
	ops     []C.db_op
	reqBuf  []byte
	outBuf  []byte
	reqPos  int32
	nOps    int
	creates int
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
	d.creates = 0
}

func (d *db) hasRoom(textBytes int) bool {
	return d.nOps < len(d.ops) && textBytes >= 0 && textBytes <= len(d.reqBuf)-int(d.reqPos)
}

func (d *db) allocText(b []byte) (int32, int32) {
	off := d.reqPos
	n := int32(len(b))
	copy(d.reqBuf[off:], b)
	d.reqPos += n
	return off, n
}

func (d *db) pushStatic(status uint16, body []byte, closeConn bool) int {
	if !d.hasRoom(len(body)) {
		return -1
	}
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
	if !d.hasRoom(0) {
		return -1
	}
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
	if !d.hasRoom(len(body) + len(user)) {
		return -1
	}
	d.creates++
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

func (d *db) execBatch(first int, nowMs, uptimeSec int64) int {
	if first < 0 || first >= d.nOps {
		return 0
	}
	return int(C.db_exec_batch(d.ctx, C.int(first), C.int(d.nOps-first), C.int64_t(nowMs), C.int64_t(uptimeSec)))
}

func (d *db) checkpointIfDue() {
	if d.ctx.wal_pages >= C.DB_CHECKPOINT_FRAMES {
		C.db_checkpoint_if_due(d.ctx)
	}
}
