; Output helpers: decimal integers, JSON string escaping, and pooled byte buffers.
%include "defs.inc"

extern malloc, realloc, free, memcpy

global w_int, w_esc, fcopy, buf_take, buf_give, buf_reserve, buf_append, part_append, part_free

section .rodata
align 16
HEXD: db "0123456789abcdef"
; escape kind per byte below 0x20: the letter for \b \t \n \f \r, 0 for \u00XX
ESCL: db 0,0,0,0,0,0,0,0,'b','t','n',0,'f','r',0,0
      db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
DIG2:
%assign i 0
%rep 100
  db '0' + i / 10, '0' + i % 10
%assign i i + 1
%endrep
align 8
POW10:
%assign i 0
%assign v 1
%rep 20
  dq v
  %assign v v * 10
%endrep
align 16
C1F: times 16 db 0x1f
CQT: times 16 db '"'
CBS: times 16 db '\'

section .bss
pool:   resq POOL_MAX
npool:  resq 1

section .text

; ---- w_int(rdi = dst, rsi = value) -> rax = end ----------------------------------------------
w_int:
  test rsi, rsi
  jns .pos
  mov byte [rdi], '-'
  inc rdi
  neg rsi
.pos:
  cmp rsi, 10
  jae .two
  lea eax, [rsi + '0']
  mov [rdi], al
  lea rax, [rdi + 1]
  ret
.two:
  lea r10, [DIG2]
  cmp rsi, 100
  jae .long
  movzx eax, word [r10 + rsi * 2]
  mov [rdi], ax
  lea rax, [rdi + 2]
  ret
.long:
  ; digit count: t = floor(bits * log10(2)), then one more if value >= 10^t
  bsr rax, rsi
  inc eax
  imul eax, eax, 1233
  shr eax, 12
  lea rcx, [POW10]
  cmp rsi, [rcx + rax * 8]
  sbb eax, -1                    ; + 1 unless value < 10^t
  lea r8, [rdi + rax]            ; end; digits are written backwards from here
  mov r11, r8
  mov rax, rsi
  mov r9, 0x28f5c28f5c28f5c3     ; (x >> 2) * r9 >> 66 = x / 100
.d2:
  cmp rax, 100
  jb .last
  mov rcx, rax
  shr rax, 2
  mul r9
  shr rdx, 2                     ; rdx = x / 100
  imul rax, rdx, 100
  sub rcx, rax                   ; x % 100
  movzx eax, word [r10 + rcx * 2]
  sub r8, 2
  mov [r8], ax
  mov rax, rdx
  jmp .d2
.last:
  cmp rax, 10
  jb .one
  movzx eax, word [r10 + rax * 2]
  mov [r8 - 2], ax
  mov rax, r11
  ret
.one:
  add al, '0'
  mov [r8 - 1], al
  mov rax, r11
  ret

; ---- w_esc(rdi = dst, rsi = src, rdx = len) -> rax = end --------------------------------------
; JSON string body: escapes '"', '\' and control characters; copies everything else as is.
; Works 16 bytes at a time: it may READ up to 15 bytes past src + len (every caller's string sits
; in a buffer with that much slack) and writes past the end like the other writers.
w_esc:
  lea r8, [rsi + rdx]
  movdqa xmm3, [C1F]
  movdqa xmm4, [CQT]
  movdqa xmm5, [CBS]
.fast:
  mov rcx, r8
  sub rcx, rsi                   ; bytes left
  jle .done
  movdqu xmm0, [rsi]
  movdqu [rdi], xmm0             ; optimistic copy
  movdqa xmm1, xmm0
  pminub xmm1, xmm3
  pcmpeqb xmm1, xmm0             ; byte <= 0x1f
  movdqa xmm2, xmm0
  pcmpeqb xmm2, xmm4
  por xmm1, xmm2
  movdqa xmm2, xmm0
  pcmpeqb xmm2, xmm5
  por xmm1, xmm2
  pmovmskb eax, xmm1
  cmp rcx, 16
  jae .full
  mov edx, 1
  shl edx, cl
  dec edx
  and eax, edx                   ; only the bytes that belong to the string
  jnz .hit
  add rdi, rcx
  mov rax, rdi
  ret
.full:
  test eax, eax
  jnz .hit
  add rsi, 16
  add rdi, 16
  jmp .fast
.hit:
  bsf eax, eax
  add rsi, rax                   ; the bytes before it are already copied
  add rdi, rax
  call .one
  jmp .fast
.done:
  mov rax, rdi
  ret
; escapes the byte at rsi, advancing both
.one:
  movzx eax, byte [rsi]
  inc rsi
  mov byte [rdi], '\'
  cmp al, 0x20
  jb .ctl
  mov [rdi + 1], al
  add rdi, 2
  ret
.ctl:
  lea rcx, [ESCL]
  movzx ecx, byte [rcx + rax]
  test ecx, ecx
  jz .u
  mov [rdi + 1], cl
  add rdi, 2
  ret
.u:
  mov dword [rdi + 1], 'u00'      ; writes "u00" and a zero byte, overwritten below
  lea rcx, [HEXD]
  mov edx, eax
  shr edx, 4
  movzx edx, byte [rcx + rdx]
  mov [rdi + 4], dl
  and eax, 15
  movzx eax, byte [rcx + rax]
  mov [rdi + 5], al
  add rdi, 6
  ret

; ---- fcopy(rdi = dst, rsi = src, rdx = len): copies exactly len bytes, reads only within src ----
fcopy:
  cmp rdx, 16
  jb .small
  lea r8, [rsi + rdx - 16]
  lea r9, [rdi + rdx - 16]
  movdqu xmm1, [r8]
.l:
  movdqu xmm0, [rsi]
  movdqu [rdi], xmm0
  add rsi, 16
  add rdi, 16
  cmp rsi, r8
  jb .l
  movdqu [r9], xmm1
  ret
.small:
  cmp edx, 8
  jb .s4
  mov rax, [rsi]
  mov rcx, [rsi + rdx - 8]
  mov [rdi], rax
  mov [rdi + rdx - 8], rcx
  ret
.s4:
  cmp edx, 4
  jb .s1
  mov eax, [rsi]
  mov ecx, [rsi + rdx - 4]
  mov [rdi], eax
  mov [rdi + rdx - 4], ecx
  ret
.s1:
  test edx, edx
  jz .r
  movzx eax, byte [rsi]
  movzx ecx, byte [rsi + rdx - 1]
  mov [rdi], al
  mov [rdi + rdx - 1], cl
  cmp edx, 2
  jbe .r
  movzx eax, byte [rsi + 1]
  mov [rdi + 1], al
.r:
  ret

; ---- buffers: [0] len, [4] cap, data from 8. Reused through a pool. ------------------------

; buf_take() -> rax = an empty buffer (OUT_CAP bytes), or 0
buf_take:
  mov rax, [npool]
  test rax, rax
  jz .new
  dec rax
  mov [npool], rax
  lea rcx, [pool]
  mov rax, [rcx + rax * 8]
  mov dword [rax + B_LEN], 0
  ret
.new:
  sub rsp, 8
  mov edi, OUT_CAP + 8
  call malloc
  add rsp, 8
  test rax, rax
  jz .r
  mov dword [rax + B_LEN], 0
  mov dword [rax + B_CAP], OUT_CAP
.r:
  ret

; buf_give(rdi = buffer or 0): back to the pool (or freed if the pool is full or it grew big)
buf_give:
  test rdi, rdi
  jz .r
  cmp dword [rdi + B_CAP], 4 * OUT_CAP
  ja .free
  mov rax, [npool]
  cmp rax, POOL_MAX
  jae .free
  lea rcx, [pool]
  mov [rcx + rax * 8], rdi
  inc rax
  mov [npool], rax
.r:
  ret
.free:
  jmp free

; buf_reserve(rdi = &slot, rsi = bytes) -> rax = buffer with cap >= len + bytes + 64, or 0.
; Takes a buffer from the pool if the slot is empty; grows it (and updates the slot) if needed.
buf_reserve:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, rsi
  mov rax, [rbx]
  test rax, rax
  jnz .have
  call buf_take
  test rax, rax
  jz .r
  mov [rbx], rax
.have:
  mov ecx, [rax + B_LEN]
  lea rdx, [rcx + r12 + 64]
  mov ecx, [rax + B_CAP]
  cmp rdx, rcx
  jbe .r
  ; grow to max(2 * cap, needed)
  lea r13, [rcx + rcx]
  cmp r13, rdx
  jae .sz
  mov r13, rdx
.sz:
  mov rdi, rax
  lea rsi, [r13 + 8]
  call realloc
  test rax, rax
  jz .r
  mov [rax + B_CAP], r13d
  mov [rbx], rax
.r:
  pop r13
  pop r12
  pop rbx
  ret

; buf_append(rdi = &slot, rsi = src, rdx = len) -> rax = buffer or 0
buf_append:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov rsi, rdx
  call buf_reserve
  test rax, rax
  jz .r
  mov ecx, [rax + B_LEN]
  lea rdi, [rax + B_DATA + rcx]
  add ecx, r13d
  mov [rax + B_LEN], ecx
  mov rsi, r12
  mov rdx, r13
  call memcpy
  mov rax, [rbx]
.r:
  pop r13
  pop r12
  pop rbx
  ret

; ---- kept input bytes: sized to what they hold (not pooled), so thousands of connections with a
; request waiting cost a few hundred bytes each ---------------------------------------------------

; part_append(rdi = &slot, rsi = src, rdx = len) -> rax = buffer or 0
part_append:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov rax, [rbx]
  test rax, rax
  jnz .have
  lea rdi, [r13 + 256]
  cmp rdi, 512
  jae .n1
  mov edi, 512
.n1:
  push rdi
  add rdi, B_DATA + 16
  sub rsp, 8
  call malloc
  add rsp, 8
  pop rcx
  test rax, rax
  jz .r
  mov dword [rax + B_LEN], 0
  mov [rax + B_CAP], ecx
  mov [rbx], rax
  jmp .copy
.have:
  mov ecx, [rax + B_LEN]
  lea rdx, [rcx + r13]
  mov ecx, [rax + B_CAP]
  cmp rdx, rcx
  jbe .copy
  lea rsi, [rcx + rcx]
  add rdx, 256
  cmp rsi, rdx
  cmovb rsi, rdx
  push rsi
  mov rdi, rax
  add rsi, B_DATA + 16
  sub rsp, 8
  call realloc
  add rsp, 8
  pop rcx
  test rax, rax
  jz .r
  mov [rax + B_CAP], ecx
  mov [rbx], rax
.copy:
  mov ecx, [rax + B_LEN]
  lea rdi, [rax + B_DATA + rcx]
  add ecx, r13d
  mov [rax + B_LEN], ecx
  mov rsi, r12
  mov rdx, r13
  call memcpy
  mov rax, [rbx]
.r:
  pop r13
  pop r12
  pop rbx
  ret

; part_free(rdi = buffer or 0)
part_free:
  test rdi, rdi
  jz .r
  jmp free
.r:
  ret
