//go:build !amd64

package main

// Elsewhere shim.c is called through cgo (see ccall_amd64.go for amd64).

/*
#include "shim.h"
*/
import "C"

import "unsafe"

func cFeed() int               { return int(C.td_feed()) }
func cPost(id int64) int       { return int(C.td_post(C.longlong(id))) }
func cLike(uid, pid int64) int { return int(C.td_like(C.longlong(uid), C.longlong(pid))) }
func cHealth() int             { return int(C.td_health()) }
func cCommit() int             { return int(C.td_commit()) }
func cTick()                   { C.td_tick() }
func cHeld() int               { return int(C.td_writes_held()) }
func cRender(kind int, p unsafe.Pointer, n int) int {
	return int(C.td_render(C.int(kind), (*C.uchar)(p), C.int(n)))
}
func cInsert(uid int64, body unsafe.Pointer, n int) int {
	return int(C.td_insert_post(C.longlong(uid), (*C.char)(body), C.int(n)))
}
