// Package ccall calls C functions without cgo's call path (see ../ccall_amd64.go for why).
package ccall

import (
	"syscall"
	"unsafe"
)

//go:noescape
func ccall(fn, a, b, c, stack uintptr) uintptr

const stackSize = 8 << 20

// The C side's stack: 8 MB, committed as it is touched, with an inaccessible lowest page so an
// overflow faults instead of writing over memory.
var stackTop = func() uintptr {
	m, err := syscall.Mmap(-1, 0, stackSize, syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_PRIVATE|syscall.MAP_ANON|syscall.MAP_NORESERVE)
	if err != nil {
		panic("ccall: " + err.Error())
	}
	if err := syscall.Mprotect(m[:4096], syscall.PROT_NONE); err != nil {
		panic("ccall: " + err.Error())
	}
	return uintptr(unsafe.Pointer(&m[stackSize-64])) &^ 15
}()

// Call runs the C function fn(a, b, c) on the C stack and returns its int result. Calls must not
// overlap (one goroutine makes them), and fn must not call back into Go.
func Call(fn, a, b, c uintptr) int {
	return int(int32(ccall(fn, a, b, c, stackTop)))
}
