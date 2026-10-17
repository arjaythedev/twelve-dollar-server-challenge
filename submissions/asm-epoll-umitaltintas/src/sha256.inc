; SHA-256 and HMAC-SHA256 (scalar, fully unrolled).
; The HMAC key is fixed for the process, so the inner and outer states after the
; ipad/opad block are computed once at startup; each token then costs 2-4 compressions.

section .rodata
align 64
sha_k:
  dd 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
  dd 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
  dd 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
  dd 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
  dd 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
  dd 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
  dd 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
  dd 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
sha_iv:
  dd 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19

section .bss
alignb 16
hmac_istate: resd 8
hmac_ostate: resd 8

section .text

; One round. a..h are 32-bit registers; i is the round number. Uses r12d-r14d.
%macro SHA_ROUND 9
  mov r12d, %5
  ror r12d, 6
  mov r13d, %5
  ror r13d, 11
  xor r12d, r13d
  ror r13d, 14
  xor r12d, r13d              ; S1(e)
  mov r13d, %6
  xor r13d, %7
  and r13d, %5
  xor r13d, %7                ; Ch(e,f,g)
  add %8, r12d
  add %8, r13d
  add %8, [r15 + 4*(%9)]        ; K[i]
  add %8, [rsp + 4*(%9)]        ; W[i]
  add %4, %8                  ; d += T1
  mov r12d, %1
  ror r12d, 2
  mov r13d, %1
  ror r13d, 13
  xor r12d, r13d
  ror r13d, 9
  xor r12d, r13d              ; S0(a)
  mov r13d, %1
  or r13d, %2
  and r13d, %3
  mov r14d, %1
  and r14d, %2
  or r13d, r14d               ; Maj(a,b,c)
  add %8, r12d
  add %8, r13d                ; h = T1 + T2 (the new a)
%endmacro

; sha256_compress(rdi = state[8], rsi = 64-byte block)
sha256_compress:
  push rbx
  push r12
  push r13
  push r14
  push r15
  push rbp
  sub rsp, 256 + 8
  mov rbp, rdi
  lea r15, [sha_k]
  ; W[0..15]
%assign i 0
%rep 16
  mov eax, [rsi + 4*i]
  bswap eax
  mov [rsp + 4*i], eax
%assign i i+1
%endrep
  ; W[16..63]
%assign i 16
%rep 48
  mov eax, [rsp + 4*(i-15)]
  mov ecx, eax
  mov edx, eax
  ror eax, 7
  ror ecx, 18
  shr edx, 3
  xor eax, ecx
  xor eax, edx                ; s0
  mov ecx, [rsp + 4*(i-2)]
  mov edx, ecx
  mov r8d, ecx
  ror ecx, 17
  ror edx, 19
  shr r8d, 10
  xor ecx, edx
  xor ecx, r8d                ; s1
  add eax, ecx
  add eax, [rsp + 4*(i-16)]
  add eax, [rsp + 4*(i-7)]
  mov [rsp + 4*i], eax
%assign i i+1
%endrep
  mov eax, [rbp]
  mov ebx, [rbp+4]
  mov ecx, [rbp+8]
  mov edx, [rbp+12]
  mov r8d, [rbp+16]
  mov r9d, [rbp+20]
  mov r10d, [rbp+24]
  mov r11d, [rbp+28]
%assign i 0
%rep 8
  SHA_ROUND eax, ebx, ecx, edx, r8d, r9d, r10d, r11d, i
  SHA_ROUND r11d, eax, ebx, ecx, edx, r8d, r9d, r10d, i+1
  SHA_ROUND r10d, r11d, eax, ebx, ecx, edx, r8d, r9d, i+2
  SHA_ROUND r9d, r10d, r11d, eax, ebx, ecx, edx, r8d, i+3
  SHA_ROUND r8d, r9d, r10d, r11d, eax, ebx, ecx, edx, i+4
  SHA_ROUND edx, r8d, r9d, r10d, r11d, eax, ebx, ecx, i+5
  SHA_ROUND ecx, edx, r8d, r9d, r10d, r11d, eax, ebx, i+6
  SHA_ROUND ebx, ecx, edx, r8d, r9d, r10d, r11d, eax, i+7
%assign i i+8
%endrep
  add [rbp], eax
  add [rbp+4], ebx
  add [rbp+8], ecx
  add [rbp+12], edx
  add [rbp+16], r8d
  add [rbp+20], r9d
  add [rbp+24], r10d
  add [rbp+28], r11d
  add rsp, 256 + 8
  pop rbp
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; sha256_finish(rdi = start state (not modified), rsi = msg, rdx = len,
;               rcx = bytes already hashed into the start state, r8 = out[32])
sha256_finish:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 32 + 128           ; state copy + 2 tail blocks
  mov r12, rsi
  mov r13, rdx
  lea r14, [rcx + rdx]        ; total length in bytes
  mov r15, r8
  movdqu xmm0, [rdi]
  movdqu xmm1, [rdi+16]
  movdqu [rsp], xmm0
  movdqu [rsp+16], xmm1
.blocks:
  cmp r13, 64
  jb .tail
  mov rdi, rsp
  mov rsi, r12
  call sha256_compress
  add r12, 64
  sub r13, 64
  jmp .blocks
.tail:
  lea rdi, [rsp+32]
  pxor xmm0, xmm0
  movdqu [rdi], xmm0
  movdqu [rdi+16], xmm0
  movdqu [rdi+32], xmm0
  movdqu [rdi+48], xmm0
  movdqu [rdi+64], xmm0
  movdqu [rdi+80], xmm0
  movdqu [rdi+96], xmm0
  movdqu [rdi+112], xmm0
  mov rsi, r12
  mov rcx, r13
  rep movsb
  mov byte [rdi], 0x80
  mov rax, r14
  shl rax, 3
  bswap rax
  lea rbx, [rsp+32+56]        ; length slot of block 1
  cmp r13, 56
  jb .one
  add rbx, 64                 ; length slot of block 2
.one:
  mov [rbx], rax
  mov rdi, rsp
  lea rsi, [rsp+32]
  call sha256_compress
  cmp r13, 56
  jb .out
  mov rdi, rsp
  lea rsi, [rsp+32+64]
  call sha256_compress
.out:
%assign i 0
%rep 8
  mov eax, [rsp + 4*i]
  bswap eax
  mov [r15 + 4*i], eax
%assign i i+1
%endrep
  add rsp, 32 + 128
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; hmac_init(rdi = key, rsi = key length)
hmac_init:
  push rbx
  push r12
  push r13
  sub rsp, 64 + 32 + 16       ; key block, hashed key
  mov r12, rdi
  mov r13, rsi
  pxor xmm0, xmm0
  movdqu [rsp], xmm0
  movdqu [rsp+16], xmm0
  movdqu [rsp+32], xmm0
  movdqu [rsp+48], xmm0
  cmp r13, 64
  jbe .short
  lea rdi, [sha_iv]           ; long keys are hashed first
  mov rsi, r12
  mov rdx, r13
  xor ecx, ecx
  mov r8, rsp
  call sha256_finish
  jmp .pads
.short:
  mov rdi, rsp
  mov rsi, r12
  mov rcx, r13
  rep movsb
.pads:
  ; inner: key ^ 0x36
  xor ecx, ecx
.x1:
  xor byte [rsp+rcx], 0x36
  inc ecx
  cmp ecx, 64
  jb .x1
  movdqu xmm0, [sha_iv]
  movdqu xmm1, [sha_iv+16]
  movdqu [hmac_istate], xmm0
  movdqu [hmac_istate+16], xmm1
  movdqu [hmac_ostate], xmm0
  movdqu [hmac_ostate+16], xmm1
  lea rdi, [hmac_istate]
  mov rsi, rsp
  call sha256_compress
  ; outer: key ^ 0x5c = (key ^ 0x36) ^ 0x6a
  xor ecx, ecx
.x2:
  xor byte [rsp+rcx], 0x6a
  inc ecx
  cmp ecx, 64
  jb .x2
  lea rdi, [hmac_ostate]
  mov rsi, rsp
  call sha256_compress
  add rsp, 64 + 32 + 16
  pop r13
  pop r12
  pop rbx
  ret

; hmac_sha256(rsi = msg, rdx = len, rdi = out[32])
hmac_sha256:
  push rbx
  sub rsp, 32
  mov rbx, rdi
  lea rdi, [hmac_istate]
  mov ecx, 64
  mov r8, rsp
  call sha256_finish          ; inner digest
  lea rdi, [hmac_ostate]
  mov rsi, rsp
  mov edx, 32
  mov ecx, 64
  mov r8, rbx
  call sha256_finish
  add rsp, 32
  pop rbx
  ret
