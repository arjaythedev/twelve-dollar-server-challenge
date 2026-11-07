#include "textflag.h"

// func ccall(fn, a, b, c, stack uintptr) uintptr
//
// Calls the C function fn(a, b, c) (System V calling convention) on the given stack, then
// switches back. R12 is callee-saved in C, so it carries the goroutine's SP across the call.
TEXT ·ccall(SB), NOSPLIT, $0-48
	MOVQ fn+0(FP), AX
	MOVQ a+8(FP), DI
	MOVQ b+16(FP), SI
	MOVQ c+24(FP), DX
	MOVQ stack+32(FP), R11
	MOVQ SP, R12
	MOVQ R11, SP
	CALL AX
	MOVQ R12, SP
	MOVQ AX, ret+40(FP)
	RET
