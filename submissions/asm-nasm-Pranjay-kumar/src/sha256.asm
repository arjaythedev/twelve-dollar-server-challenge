; SHA-256, HMAC-SHA256 (with the key's padded blocks hashed once at startup) and base64url.
%include "defs.inc"

global sha256_block, sha256_finish, hmac_init, hmac_verify, b64url_dec

section .rodata
align 64
K256:
  dd 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
  dd 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
  dd 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
  dd 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
  dd 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
  dd 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
  dd 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
  dd 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
IV256:
  dd 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19

; base64url alphabet -> value, 0xff for anything else
B64DEC:
%assign i 0
%rep 256
  %if i >= 'A' && i <= 'Z'
    db i - 'A'
  %elif i >= 'a' && i <= 'z'
    db i - 'a' + 26
  %elif i >= '0' && i <= '9'
    db i - '0' + 52
  %elif i == '-'
    db 62
  %elif i == '_'
    db 63
  %else
    db 0xff
  %endif
  %assign i i + 1
%endrep

section .bss
alignb 16
hmac_istate: resd 8
hmac_ostate: resd 8

section .text

; ---- sha256_block(rdi = state[8], rsi = 64-byte block) ---------------------------------------
%macro SHA_ROUND 9   ; a b c d e f g h i
  mov eax, %5
  ror eax, 6
  mov ebx, %5
  ror ebx, 11
  xor eax, ebx
  ror ebx, 14
  xor eax, ebx            ; S1(e)
  mov ebx, %6
  xor ebx, %7
  and ebx, %5
  xor ebx, %7             ; Ch(e,f,g)
  add eax, ebx
  add eax, [K256 + 4 * %9]
  add eax, [rsp + 4 * %9]
  add %8, eax             ; h += T1
  add %4, %8              ; d += T1
  mov eax, %1
  ror eax, 2
  mov ebx, %1
  ror ebx, 13
  xor eax, ebx
  ror ebx, 9
  xor eax, ebx            ; S0(a)
  mov ebx, %1
  or ebx, %2
  and ebx, %3
  mov ecx, %1
  and ecx, %2
  or ebx, ecx             ; Maj(a,b,c)
  add eax, ebx
  add %8, eax             ; h = T1 + T2
%endmacro

sha256_block:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 264
%assign i 0
%rep 16
  mov eax, [rsi + 4 * i]
  bswap eax
  mov [rsp + 4 * i], eax
%assign i i + 1
%endrep
  mov ecx, 16
.expand:
  mov eax, [rsp + rcx * 4 - 8]     ; W[i-2]
  mov ebx, eax
  mov edx, eax
  ror eax, 17
  ror ebx, 19
  shr edx, 10
  xor eax, ebx
  xor eax, edx                      ; s1
  mov ebx, [rsp + rcx * 4 - 60]    ; W[i-15]
  mov edx, ebx
  mov ebp, ebx
  ror ebx, 7
  ror edx, 18
  shr ebp, 3
  xor ebx, edx
  xor ebx, ebp                      ; s0
  add eax, ebx
  add eax, [rsp + rcx * 4 - 28]    ; W[i-7]
  add eax, [rsp + rcx * 4 - 64]    ; W[i-16]
  mov [rsp + rcx * 4], eax
  inc ecx
  cmp ecx, 64
  jb .expand

  mov r8d, [rdi]
  mov r9d, [rdi + 4]
  mov r10d, [rdi + 8]
  mov r11d, [rdi + 12]
  mov r12d, [rdi + 16]
  mov r13d, [rdi + 20]
  mov r14d, [rdi + 24]
  mov r15d, [rdi + 28]
%xdefine VA r8d
%xdefine VB r9d
%xdefine VC r10d
%xdefine VD r11d
%xdefine VE r12d
%xdefine VF r13d
%xdefine VG r14d
%xdefine VH r15d
%assign i 0
%rep 64
  SHA_ROUND VA, VB, VC, VD, VE, VF, VG, VH, i
  %xdefine VT VH
  %xdefine VH VG
  %xdefine VG VF
  %xdefine VF VE
  %xdefine VE VD
  %xdefine VD VC
  %xdefine VC VB
  %xdefine VB VA
  %xdefine VA VT
%assign i i + 1
%endrep
  ; after 64 rotations (a multiple of 8) the names are back in place
  add [rdi], r8d
  add [rdi + 4], r9d
  add [rdi + 8], r10d
  add [rdi + 12], r11d
  add [rdi + 16], r12d
  add [rdi + 20], r13d
  add [rdi + 24], r14d
  add [rdi + 28], r15d
  add rsp, 264
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; ---- sha256_finish(rdi = state, rsi = msg, rdx = len, rcx = bytes already hashed into state) ---
; Hashes msg and the padding; the state then holds the digest (as host-order words).
sha256_finish:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 128 + 16             ; 5 pushes + 144 -> aligned
  mov r12, rdi
  mov r13, rsi
  mov r14, rdx
  lea r15, [rcx + rdx]          ; total bytes
.blocks:
  cmp r14, 64
  jb .tail
  mov rdi, r12
  mov rsi, r13
  call sha256_block
  add r13, 64
  sub r14, 64
  jmp .blocks
.tail:
  ; copy the rest, then 0x80, zeros, and the bit length (big-endian) at the end of 1 or 2 blocks
  pxor xmm0, xmm0
  movdqu [rsp], xmm0
  movdqu [rsp + 16], xmm0
  movdqu [rsp + 32], xmm0
  movdqu [rsp + 48], xmm0
  movdqu [rsp + 64], xmm0
  movdqu [rsp + 80], xmm0
  movdqu [rsp + 96], xmm0
  movdqu [rsp + 112], xmm0
  xor ecx, ecx
.copy:
  cmp rcx, r14
  jae .copied
  mov al, [r13 + rcx]
  mov [rsp + rcx], al
  inc rcx
  jmp .copy
.copied:
  mov byte [rsp + rcx], 0x80
  mov rax, r15
  shl rax, 3
  bswap rax
  mov ebx, 64
  cmp r14, 56
  jb .one
  mov ebx, 128
.one:
  mov [rsp + rbx - 8], rax
  mov rdi, r12
  mov rsi, rsp
  call sha256_block
  cmp ebx, 128
  jne .done
  mov rdi, r12
  lea rsi, [rsp + 64]
  call sha256_block
.done:
  add rsp, 144
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ---- hmac_init(rdi = key, rsi = len): hashes the key's inner and outer padded blocks ----------
hmac_init:
  push rbx
  push r12
  push r13
  sub rsp, 64 + 32 + 32 + 32 + 16 ; [0,64) key block, [64,96) state, [96,224) scratch block
  mov r12, rdi
  mov r13, rsi
  pxor xmm0, xmm0
  movdqu [rsp], xmm0
  movdqu [rsp + 16], xmm0
  movdqu [rsp + 32], xmm0
  movdqu [rsp + 48], xmm0
  cmp r13, 64
  jbe .short
  ; long key: K = SHA-256(key)
  movdqu xmm0, [IV256]
  movdqu xmm1, [IV256 + 16]
  movdqu [rsp + 64], xmm0
  movdqu [rsp + 80], xmm1
  lea rdi, [rsp + 64]
  mov rsi, r12
  mov rdx, r13
  xor ecx, ecx
  call sha256_finish
  xor ecx, ecx
.kw:
  mov eax, [rsp + 64 + rcx * 4]
  bswap eax
  mov [rsp + rcx * 4], eax
  inc ecx
  cmp ecx, 8
  jb .kw
  jmp .pads
.short:
  xor ecx, ecx
.kc:
  cmp rcx, r13
  jae .pads
  mov al, [r12 + rcx]
  mov [rsp + rcx], al
  inc rcx
  jmp .kc
.pads:
  ; inner: state = IV, block = key ^ 0x36
  xor ecx, ecx
.ip:
  mov al, [rsp + rcx]
  xor al, 0x36
  mov [rsp + 96 + rcx], al
  inc ecx
  cmp ecx, 64
  jb .ip
  movdqu xmm0, [IV256]
  movdqu xmm1, [IV256 + 16]
  movdqu [hmac_istate], xmm0
  movdqu [hmac_istate + 16], xmm1
  lea rdi, [hmac_istate]
  lea rsi, [rsp + 96]
  call sha256_block
  xor ecx, ecx
.op:
  mov al, [rsp + rcx]
  xor al, 0x5c
  mov [rsp + 96 + rcx], al
  inc ecx
  cmp ecx, 64
  jb .op
  movdqu xmm0, [IV256]
  movdqu xmm1, [IV256 + 16]
  movdqu [hmac_ostate], xmm0
  movdqu [hmac_ostate + 16], xmm1
  lea rdi, [hmac_ostate]
  lea rsi, [rsp + 96]
  call sha256_block
  add rsp, 176
  pop r13
  pop r12
  pop rbx
  ret

; ---- hmac_verify(rdi = msg, rsi = len, rdx = 32-byte signature) -> eax 1 if it matches --------
hmac_verify:
  push rbx
  push r12
  push r13
  sub rsp, 96                    ; [0,32) inner state, [32,64) digest bytes, [64,96) outer state
  mov r12, rdi
  mov r13, rsi
  mov rbx, rdx
  movdqu xmm0, [hmac_istate]
  movdqu xmm1, [hmac_istate + 16]
  movdqu [rsp], xmm0
  movdqu [rsp + 16], xmm1
  mov rdi, rsp
  mov rsi, r12
  mov rdx, r13
  mov ecx, 64
  call sha256_finish
  xor ecx, ecx
.d:
  mov eax, [rsp + rcx * 4]
  bswap eax
  mov [rsp + 32 + rcx * 4], eax
  inc ecx
  cmp ecx, 8
  jb .d
  movdqu xmm0, [hmac_ostate]
  movdqu xmm1, [hmac_ostate + 16]
  movdqu [rsp + 64], xmm0
  movdqu [rsp + 80], xmm1
  lea rdi, [rsp + 64]
  lea rsi, [rsp + 32]
  mov edx, 32
  mov ecx, 64
  call sha256_finish
  xor ecx, ecx
  xor edx, edx
.c:                              ; constant-time compare
  mov eax, [rsp + 64 + rcx * 4]
  bswap eax
  xor eax, [rbx + rcx * 4]
  or edx, eax
  inc ecx
  cmp ecx, 8
  jb .c
  xor eax, eax
  test edx, edx
  sete al
  add rsp, 96
  pop r13
  pop r12
  pop rbx
  ret

; ---- b64url_dec(rdi = src, rsi = len, rdx = dst) -> rax = bytes written, or -1 ----------------
; Unpadded base64url; only the canonical spelling (unused low bits zero) is accepted.
b64url_dec:
  push rbx
  push r12
  mov r8, rdx                    ; out
  lea r9, [B64DEC]
  mov r12, rsi
  and r12d, 3                    ; characters after the full quads
  cmp r12d, 1
  je .bad
  mov r10, rsi
  shr r10, 2                     ; full quads
.quad:
  test r10, r10
  jz .rest
  movzx eax, byte [rdi]
  movzx eax, byte [r9 + rax]
  movzx ecx, byte [rdi + 1]
  movzx ecx, byte [r9 + rcx]
  movzx ebx, byte [rdi + 2]
  movzx ebx, byte [r9 + rbx]
  movzx r11d, byte [rdi + 3]
  movzx r11d, byte [r9 + r11]
  mov esi, eax
  or esi, ecx
  or esi, ebx
  or esi, r11d
  test esi, 0xc0
  jnz .bad
  shl eax, 18
  shl ecx, 12
  shl ebx, 6
  or eax, ecx
  or eax, ebx
  or eax, r11d
  mov ecx, eax
  shr ecx, 16
  mov [r8 + 2], al
  shr eax, 8
  mov [r8 + 1], al
  mov [r8], cl
  add r8, 3
  add rdi, 4
  dec r10
  jmp .quad
.rest:
  test r12d, r12d
  jz .ok
  movzx ecx, byte [rdi]
  movzx ecx, byte [r9 + rcx]
  movzx r11d, byte [rdi + 1]
  movzx r11d, byte [r9 + r11]
  mov esi, ecx
  or esi, r11d
  shl ecx, 18
  shl r11d, 12
  or ecx, r11d
  cmp r12d, 3
  jne .two
  movzx r11d, byte [rdi + 2]
  movzx r11d, byte [r9 + r11]
  or esi, r11d
  test esi, 0xc0
  jnz .bad
  shl r11d, 6
  or ecx, r11d
  test ecx, 0xff
  jnz .bad
  mov eax, ecx
  shr eax, 16
  mov [r8], al
  shr ecx, 8
  mov [r8 + 1], cl
  add r8, 2
  jmp .ok
.two:
  test esi, 0xc0
  jnz .bad
  test ecx, 0xffff
  jnz .bad
  shr ecx, 16
  mov [r8], cl
  inc r8
.ok:
  mov rax, r8
  sub rax, rdx
  pop r12
  pop rbx
  ret
.bad:
  mov rax, -1
  pop r12
  pop rbx
  ret
