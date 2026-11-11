#include "textflag.h"

DATA quote<>+0(SB)/8, $0x2222222222222222
DATA quote<>+8(SB)/8, $0x2222222222222222
GLOBL quote<>(SB), RODATA|NOPTR, $16
DATA slash<>+0(SB)/8, $0x5c5c5c5c5c5c5c5c
DATA slash<>+8(SB)/8, $0x5c5c5c5c5c5c5c5c
GLOBL slash<>(SB), RODATA|NOPTR, $16
DATA control<>+0(SB)/8, $0x1f1f1f1f1f1f1f1f
DATA control<>+8(SB)/8, $0x1f1f1f1f1f1f1f1f
GLOBL control<>(SB), RODATA|NOPTR, $16

// SSE2 is required by the amd64 baseline. No speculative or overlapping loads:
// a full 16-byte source/destination range must remain before each vector copy.
TEXT ·copyPlain(SB), NOSPLIT, $0-32
	MOVQ dst+0(FP), DI
	MOVQ src+8(FP), SI
	MOVQ n+16(FP), CX
	XORQ AX, AX
	CMPQ CX, $16
	JB tail
	MOVOU quote<>(SB), X4
	MOVOU slash<>(SB), X5
	MOVOU control<>(SB), X6
	PXOR X7, X7
vector:
	MOVOU (SI)(AX*1), X0
	MOVOU X0, X1
	MOVOU X0, X2
	MOVOU X0, X3
	PCMPEQB X4, X1
	PCMPEQB X5, X2
	PSUBUSB X6, X3
	PCMPEQB X7, X3
	POR X2, X1
	POR X3, X1
	PMOVMSKB X1, DX
	TESTQ DX, DX
	JNZ tail
	MOVOU X0, (DI)(AX*1)
	ADDQ $16, AX
	SUBQ $16, CX
	CMPQ CX, $16
	JAE vector
tail:
	TESTQ CX, CX
	JZ done
	MOVBLZX (SI)(AX*1), DX
	CMPB DL, $0x20
	JB done
	CMPB DL, $0x22
	JE done
	CMPB DL, $0x5c
	JE done
	MOVB DL, (DI)(AX*1)
	INCQ AX
	DECQ CX
	JMP tail
done:
	MOVQ AX, ret+24(FP)
	RET
