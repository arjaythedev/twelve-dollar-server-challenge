; HTTP/1.1 request framing. Looks only at what the API uses: the request line, Authorization,
; Content-Length (and Transfer-Encoding, which is refused).
%include "defs.inc"

global http_parse, find_byte

section .rodata
S_AUTH: db "authorization"
S_CLEN: db "content-length"
S_TENC: db "transfer-encoding"

section .text

; ---- find_byte(rdi = p, rsi = end, edx = byte) -> rax = pointer to it, or 0 -------------------
find_byte:
  movd xmm1, edx
  punpcklbw xmm1, xmm1
  punpcklwd xmm1, xmm1
  pshufd xmm1, xmm1, 0
.v:
  lea rax, [rdi + 16]
  cmp rax, rsi
  ja .s
  movdqu xmm0, [rdi]
  pcmpeqb xmm0, xmm1
  pmovmskb eax, xmm0
  test eax, eax
  jnz .hit
  add rdi, 16
  jmp .v
.hit:
  bsf eax, eax
  add rax, rdi
  ret
.s:
  cmp rdi, rsi
  jae .no
  cmp [rdi], dl
  je .f
  inc rdi
  jmp .s
.f:
  mov rax, rdi
  ret
.no:
  xor eax, eax
  ret

; compares rcx bytes at rsi (any case) with lowercase rdi; ZF set if equal
%macro ICASE_EQ 0
  push rcx
%%l:
  test rcx, rcx
  jz %%y
  dec rcx
  movzx eax, byte [rsi + rcx]
  or al, 0x20
  cmp al, [rdi + rcx]
  jne %%n
  jmp %%l
%%y:
  xor eax, eax                   ; ZF = 1
%%n:
  pop rcx
%endmacro

; ---- http_parse(rdi = buf, rsi = len, rdx = request) -> eax 1 complete, 0 partial, -1 bad ------
http_parse:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov r12, rdi                   ; buf
  lea r13, [rdi + rsi]           ; end
  mov r14, rdx                   ; request
  xor eax, eax
  mov [r14 + Q_AUTH], rax
  mov [r14 + Q_ALEN], rax
  mov [r14 + Q_BLEN], rax
  xor ebp, ebp                   ; content length
  ; request line
  mov rdi, r12
  mov rsi, r13
  mov edx, 10
  call find_byte
  test rax, rax
  jz .partial
  mov r15, rax                   ; '\n'
  mov rbx, rax
  cmp rbx, r12
  je .bad
  cmp byte [rbx - 1], 13
  jne .l1
  dec rbx                        ; rbx = end of line
.l1:
  mov rdi, r12
  mov rsi, rbx
  mov edx, ' '
  call find_byte
  test rax, rax
  jz .bad
  cmp rax, r12
  je .bad
  ; method
  mov rcx, rax
  sub rcx, r12
  mov qword [r14 + Q_METHOD], 2
  cmp rcx, 3
  jne .m4
  cmp word [r12], 'GE'
  jne .mdone
  cmp byte [r12 + 2], 'T'
  jne .mdone
  mov qword [r14 + Q_METHOD], 0
  jmp .mdone
.m4:
  cmp rcx, 4
  jne .mdone
  cmp dword [r12], 'POST'
  jne .mdone
  mov qword [r14 + Q_METHOD], 1
.mdone:
  lea rdi, [rax + 1]             ; target
  push rdi
  mov rsi, rbx
  mov edx, ' '
  sub rsp, 8
  call find_byte
  add rsp, 8
  pop rdi
  test rax, rax
  jz .bad
  cmp rax, rdi
  je .bad
  ; "HTTP/1." after it
  lea rcx, [rax + 8]
  cmp rcx, rbx
  ja .bad
  cmp dword [rax + 1], 'HTTP'
  jne .bad
  cmp word [rax + 5], '/1'
  jne .bad
  cmp byte [rax + 7], '.'
  jne .bad
  ; path = target up to '?'
  mov [r14 + Q_PATH], rdi
  mov rsi, rax
  push rax
  push rdi
  mov edx, '?'
  call find_byte
  pop rdi
  pop rcx
  test rax, rax
  cmovz rax, rcx
  sub rax, rdi
  mov [r14 + Q_PLEN], rax
  ; headers
  lea r15, [r15 + 1]             ; start of the next line
.hdr:
  mov rdi, r15
  mov rsi, r13
  mov edx, 10
  call find_byte
  test rax, rax
  jz .partial
  mov rbx, rax                   ; '\n'
  lea rcx, [rax + 1]
  push rcx                       ; next line
  sub rsp, 8
  cmp rbx, r15
  je .eol
  cmp byte [rbx - 1], 13
  jne .eol
  dec rbx
.eol:
  cmp rbx, r15
  je .headend
  mov rdi, r15
  mov rsi, rbx
  mov edx, ':'
  call find_byte
  test rax, rax
  jz .hnext
  mov rcx, rax
  sub rcx, r15                   ; name length
  mov rsi, r15
  cmp rcx, 13
  je .h13
  cmp rcx, 14
  je .h14
  cmp rcx, 17
  je .h17
  jmp .hnext
.h13:
  lea rdi, [S_AUTH]
  mov r8, rax
  ICASE_EQ
  jne .hnext
  mov rdi, r8
  inc rdi
  mov rsi, rbx
  call trim
  mov [r14 + Q_AUTH], rdi
  sub rsi, rdi
  mov [r14 + Q_ALEN], rsi
  jmp .hnext
.h14:
  lea rdi, [S_CLEN]
  mov r8, rax
  ICASE_EQ
  jne .hnext
  mov rdi, r8
  inc rdi
  mov rsi, rbx
  call trim
  mov rcx, rsi
  sub rcx, rdi
  jz .bad2
  cmp rcx, 9
  ja .bad2
  xor ebp, ebp
.cl:
  movzx eax, byte [rdi]
  sub eax, '0'
  cmp eax, 9
  ja .bad2
  imul rbp, rbp, 10
  add rbp, rax
  inc rdi
  cmp rdi, rsi
  jb .cl
  cmp rbp, MAX_BODY
  ja .bad2
  jmp .hnext
.h17:
  lea rdi, [S_TENC]
  ICASE_EQ
  je .bad2
.hnext:
  add rsp, 8
  pop r15
  jmp .hdr
.headend:
  add rsp, 8
  pop r15                        ; first byte of the body
  mov rax, r13
  sub rax, r15
  cmp rax, rbp
  jb .partial_body
  mov [r14 + Q_BODY], r15
  mov [r14 + Q_BLEN], rbp
  lea rax, [r15 + rbp]
  sub rax, r12
  mov [r14 + Q_USED], rax
  mov eax, 1
  jmp .ret
.partial_body:
  xor eax, eax
  jmp .ret
.bad2:
  add rsp, 16
.bad:
  mov eax, -1
  jmp .ret
.partial:
  mov rax, r13
  sub rax, r12
  cmp rax, MAX_HEAD
  ja .bad
  xor eax, eax
.ret:
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; trims spaces and tabs: rdi = start, rsi = end (both updated)
trim:
.l:
  cmp rdi, rsi
  jae .r
  cmp byte [rdi], ' '
  je .il
  cmp byte [rdi], 9
  jne .r
.il:
  inc rdi
  jmp .l
.r:
  cmp rsi, rdi
  jbe .d
  cmp byte [rsi - 1], ' '
  je .ir
  cmp byte [rsi - 1], 9
  jne .d
.ir:
  dec rsi
  jmp .r
.d:
  ret
