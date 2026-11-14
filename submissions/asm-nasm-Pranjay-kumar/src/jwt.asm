; HS256 JWT verification. Every request's signature is checked (rule 5).
%include "defs.inc"

extern hmac_verify, b64url_dec, json_obj, json_unescape, find_byte, strtod, clock_gettime

global jwt_verify, parse_positive, jwt_uid, jwt_uname, jwt_ulen

section .rodata
K_ALG:  db "alg"
K_EXP:  db "exp"
K_SUB:  db "sub"
K_USER: db "username"
align 8
KEYS_HDR: dq K_ALG, 3
KEYS_PAY: dq K_EXP, 3, K_SUB, 3, K_USER, 8

section .bss
alignb 16
jbuf:      resb MAX_HEAD + 64
jwt_uname: resb MAX_HEAD + 64
jwt_ulen:  resq 1
jwt_uid:   resq 1
sigbuf:    resb 64
numbuf:    resb 80
slots:     resb 3 * J_SIZE
ts:        resq 2
dots:      resq 2

section .text

; ---- parse_positive(rdi = p, rsi = len) -> rax = value, or 0 if not a positive integer ---------
; Decimal digits only, no sign or leading zero, at most 18 digits.
parse_positive:
  xor eax, eax
  test rsi, rsi
  jz .bad
  cmp rsi, 18
  ja .bad
  cmp byte [rdi], '0'
  je .bad
  xor ecx, ecx
.l:
  movzx edx, byte [rdi + rcx]
  sub edx, '0'
  cmp edx, 9
  ja .bad
  imul rax, rax, 10
  add rax, rdx
  inc rcx
  cmp rcx, rsi
  jb .l
  ret
.bad:
  xor eax, eax
  ret

; ---- jwt_verify(rdi = Authorization value or 0, rsi = len) -> eax ------------------------------
; 0 ok (jwt_uid, jwt_uname/jwt_ulen set), 1 missing bearer token, 2 invalid or expired,
; 3 invalid payload.
jwt_verify:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov eax, 1
  test rdi, rdi
  jz .ret
  cmp rsi, 7
  jb .ret
  cmp dword [rdi], 'Bear'
  jne .ret
  cmp word [rdi + 4], 'er'
  jne .ret
  cmp byte [rdi + 6], ' '
  jne .ret
  lea r12, [rdi + 7]             ; token
  lea r13, [rdi + rsi]           ; end
  ; exactly three parts
  mov rdi, r12
  mov rsi, r13
  mov edx, '.'
  call find_byte
  test rax, rax
  jz .invalid
  mov r14, rax                   ; first dot
  lea rdi, [rax + 1]
  mov rsi, r13
  mov edx, '.'
  call find_byte
  test rax, rax
  jz .invalid
  mov r15, rax                   ; second dot
  lea rdi, [rax + 1]
  mov rsi, r13
  mov edx, '.'
  call find_byte
  test rax, rax
  jnz .invalid
  ; signature: 43 characters, 32 bytes
  mov rsi, r13
  sub rsi, r15
  dec rsi
  cmp rsi, 43
  jne .invalid
  lea rdi, [r15 + 1]
  lea rdx, [sigbuf]
  call b64url_dec
  cmp rax, 32
  jne .invalid
  mov rdi, r12
  mov rsi, r15
  sub rsi, r12
  lea rdx, [sigbuf]
  call hmac_verify
  test eax, eax
  jz .invalid
  ; header: an object whose alg is "HS256"
  mov rdi, r12
  mov rsi, r14
  sub rsi, r12
  lea rdx, [jbuf]
  call b64url_dec
  test rax, rax
  js .invalid
  lea rdi, [jbuf]
  mov rsi, rax
  lea rdx, [KEYS_HDR]
  lea rcx, [slots]
  mov r8d, 1
  call json_obj
  cmp eax, 1
  jne .invalid
  lea rbx, [slots]
  cmp qword [rbx + J_TYPE], 1
  jne .invalid
  mov rdi, [rbx + J_START]
  mov rsi, [rbx + J_LEN]
  cmp qword [rbx + J_ESC], 0
  je .alg
  cmp rsi, 64
  ja .invalid
  mov rdx, rsi
  mov rsi, rdi
  lea rdi, [numbuf]
  call json_unescape
  lea rdi, [numbuf]
  mov rsi, rax
  sub rsi, rdi
.alg:
  cmp rsi, 5
  jne .invalid
  cmp dword [rdi], 'HS25'
  jne .invalid
  cmp byte [rdi + 4], '6'
  jne .invalid
  ; payload
  lea rdi, [r14 + 1]
  mov rsi, r15
  sub rsi, rdi
  lea rdx, [jbuf]
  call b64url_dec
  test rax, rax
  js .invalid
  lea rdi, [jbuf]
  mov rsi, rax
  lea rdx, [KEYS_PAY]
  lea rcx, [slots]
  mov r8d, 3
  call json_obj
  cmp eax, 1
  jne .invalid
  ; exp: a number, and now < exp
  lea rbx, [slots]
  cmp qword [rbx + J_TYPE], 2
  jne .invalid
  mov rcx, [rbx + J_LEN]
  cmp rcx, 63
  ja .invalid
  mov rsi, [rbx + J_START]
  lea rdi, [numbuf]
  rep movsb
  mov byte [rdi], 0
  lea rdi, [numbuf]
  xor esi, esi
  call strtod
  movq r12, xmm0
  xor edi, edi                   ; CLOCK_REALTIME
  lea rsi, [ts]
  call clock_gettime
  cvtsi2sd xmm0, qword [ts]
  cvtsi2sd xmm1, qword [ts + 8]
  mulsd xmm1, [NS]
  addsd xmm0, xmm1               ; now, seconds
  movq xmm1, r12                 ; exp
  ucomisd xmm1, xmm0
  jbe .invalid                   ; exp <= now (or NaN): expired
  ; sub: a string holding a positive integer
  cmp qword [rbx + J_SIZE + J_TYPE], 1
  jne .payload
  mov rdi, [rbx + J_SIZE + J_START]
  mov rsi, [rbx + J_SIZE + J_LEN]
  cmp qword [rbx + J_SIZE + J_ESC], 0
  je .sub
  cmp rsi, 64
  ja .payload
  mov rdx, rsi
  mov rsi, rdi
  lea rdi, [numbuf]
  call json_unescape
  lea rdi, [numbuf]
  mov rsi, rax
  sub rsi, rdi
.sub:
  call parse_positive
  test rax, rax
  jz .payload
  mov [jwt_uid], rax
  ; username: a string, used as is
  cmp qword [rbx + 2 * J_SIZE + J_TYPE], 1
  jne .payload
  mov rsi, [rbx + 2 * J_SIZE + J_START]
  mov rdx, [rbx + 2 * J_SIZE + J_LEN]
  lea rdi, [jwt_uname]
  call json_unescape
  lea rcx, [jwt_uname]
  sub rax, rcx
  mov [jwt_ulen], rax
  xor eax, eax
  jmp .ret
.payload:
  mov eax, 3
  jmp .ret
.invalid:
  mov eax, 2
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

section .rodata
align 8
NS: dq 1.0e-9
