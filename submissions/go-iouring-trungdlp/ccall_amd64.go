package main

// Calls into shim.c without cgo's call path. A cgo call marks the thread as in a system call;
// with one P, Go's sysmon thread takes the P back from any such call it sees twice (20 µs apart),
// starts polling every 20 µs again, and the main thread has to win the P back when C returns.
// Near saturation, when interrupts stretch C calls, that came to 13% of the CPU in a 1 vCPU VM.
// Here the C function runs on a stack of its own through a small assembly trampoline (package
// ccall), and the goroutine never leaves the running state. Only the event-loop goroutine calls
// in; shim.c's functions don't call back into Go and don't block for long.

/*
#include "shim.h"
*/
import "C"

import (
	"unsafe"

	"feedserver/ccall"
)

var (
	fnFeed   = uintptr(unsafe.Pointer(C.td_feed))
	fnPost   = uintptr(unsafe.Pointer(C.td_post))
	fnRender = uintptr(unsafe.Pointer(C.td_render))
	fnLike   = uintptr(unsafe.Pointer(C.td_like))
	fnInsert = uintptr(unsafe.Pointer(C.td_insert_post))
	fnHealth = uintptr(unsafe.Pointer(C.td_health))
	fnCommit = uintptr(unsafe.Pointer(C.td_commit))
	fnTick   = uintptr(unsafe.Pointer(C.td_tick))
	fnHeld   = uintptr(unsafe.Pointer(C.td_writes_held))
)

func call3(fn, a, b, c uintptr) int { return ccall.Call(fn, a, b, c) }

func cFeed() int               { return call3(fnFeed, 0, 0, 0) }
func cPost(id int64) int       { return call3(fnPost, uintptr(id), 0, 0) }
func cLike(uid, pid int64) int { return call3(fnLike, uintptr(uid), uintptr(pid), 0) }
func cHealth() int             { return call3(fnHealth, 0, 0, 0) }
func cCommit() int             { return call3(fnCommit, 0, 0, 0) }
func cTick()                   { call3(fnTick, 0, 0, 0) }
func cHeld() int               { return call3(fnHeld, 0, 0, 0) }
func cRender(kind int, p unsafe.Pointer, n int) int {
	return call3(fnRender, uintptr(kind), uintptr(p), uintptr(n))
}
func cInsert(uid int64, body unsafe.Pointer, n int) int {
	return call3(fnInsert, uintptr(uid), uintptr(body), uintptr(n))
}
