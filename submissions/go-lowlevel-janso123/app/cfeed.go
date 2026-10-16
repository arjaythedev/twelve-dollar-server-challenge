//go:build cgo_sqlite && !pure_sqlite

package main

/*
#include "cfeed.h"
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"unsafe"
)

type cReader struct {
	ptr *C.cf_reader
	buf []byte
}

func newCReader(path, feedSQL, postSQL string) (*cReader, error) {
	p, f, s := C.CString(path), C.CString(feedSQL), C.CString(postSQL)
	defer C.free(unsafe.Pointer(p))
	defer C.free(unsafe.Pointer(f))
	defer C.free(unsafe.Pointer(s))
	r := C.cf_open(p, f, s)
	if r == nil {
		return nil, errors.New("C SQLite reader initialization failed (including version/pragmas/prepare)")
	}
	return &cReader{ptr: r, buf: make([]byte, 65536)}, nil
}

func (r *cReader) close() {
	if r != nil && r.ptr != nil {
		C.cf_close(r.ptr)
		r.ptr = nil
	}
}

func (r *cReader) read(feed bool, id int64) reply {
	for {
		var used C.size_t
		kind := C.int(0)
		if feed {
			kind = 1
		}
		status := C.cf_read(r.ptr, kind, C.int64_t(id), (*C.char)(unsafe.Pointer(&r.buf[0])), C.size_t(len(r.buf)), &used)
		switch status {
		case C.CF_OK:
			// A reply is consumed by a different goroutine after this actor may
			// start its next job. Never hand its reusable storage to HTTP.
			return reply{200, append([]byte(nil), r.buf[:int(used)]...)}
		case C.CF_NOT_FOUND:
			return failure(404, "post not found")
		case C.CF_TOO_SMALL:
			if uint64(used) > uint64(int(^uint(0)>>1)) {
				return failure(500, "internal server error")
			}
			r.buf = make([]byte, int(used))
		default:
			return failure(500, "internal server error")
		}
	}
}

func cQuoteForTest(input string) []byte {
	buf := make([]byte, len(input)*6+2)
	var source *C.char
	if len(input) > 0 {
		source = (*C.char)(unsafe.Pointer(unsafe.StringData(input)))
	}
	n := C.cf_quote(source, C.size_t(len(input)), (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return buf[:int(n)]
}
