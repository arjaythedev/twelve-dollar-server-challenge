; JWT verification: HS256 only, signature and exp checked before payload.

%include "common.inc"

extern b64url_decode
extern hmac_verify
extern json_parse_alg
extern json_parse_payload
extern starts_with
extern trim_ws
extern u64_parse
extern mem_eq

section .rodata
jwt_hs256:  db 'HS256'
jwt_bearer: db 'Bearer '
jwt_dot:    db '.'

section .text

global jwt_auth
global jwt_verify

%define JV_HDR   0
%define JV_PAY   4096
%define JV_SIG   12288
%define JV_ALG   12320
%define JV_SUB   12352

; -----------------------------------------------------------------------------
; jwt_auth(secret, secretlen, authptr, authlen, out, unamebuf)
;   -> eax = 0 ok / 1 missing bearer / 2 invalid or expired / 3 invalid payload
; -----------------------------------------------------------------------------
jwt_auth:
    test rdx, rdx
    jz .missing
    cmp rcx, 7
    jb .missing
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov r13, rsi
    mov r14, r8
    mov r15, r9
    mov rbx, rdx
    mov [rbp-56], rcx
    mov rdi, rdx
    mov rsi, rcx
    lea rdx, [rel jwt_bearer]
    mov ecx, 7
    call starts_with
    test eax, eax
    jz .missing_frame
    add rbx, 7
    mov rsi, [rbp-56]
    sub rsi, 7
    mov rdi, rbx
    call trim_ws
    test rdx, rdx
    jz .invalid_frame
    mov rbx, rdx                ; trimmed token length
    mov rdx, rax                ; trimmed token pointer
    mov rcx, rbx
    mov rdi, r12
    mov rsi, r13
    mov r8, r14
    mov r9, r15
    call jwt_verify
    jmp .out
.missing_frame:
    mov eax, 1
    jmp .out
.invalid_frame:
    mov eax, 2
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.missing:
    mov eax, 1
    ret

; -----------------------------------------------------------------------------
; jwt_verify(secret, secretlen, token, toklen, out, unamebuf)
;   -> eax = 0 ok / 2 invalid or expired / 3 invalid payload
; -----------------------------------------------------------------------------
jwt_verify:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16408
    mov [rbp-56], rdi           ; secret
    mov [rbp-64], rsi           ; secretlen
    mov [rbp-72], rdx           ; token
    mov [rbp-80], rcx           ; toklen
    mov [rbp-88], r8            ; out
    mov [rbp-96], r9            ; unamebuf
    cmp rcx, 8192
    ja .invalid
    mov r12, rdx                ; token
    mov r13, rcx                ; toklen
    ; first dot
    mov rdi, r12
    mov rsi, r13
    mov edx, '.'
    call find_dot
    cmp rax, -1
    je .invalid
    mov r14, rax                ; d1
    ; second dot
    lea rdi, [r12+r14+1]
    mov rsi, r13
    sub rsi, r14
    dec rsi
    mov edx, '.'
    call find_dot
    cmp rax, -1
    je .invalid
    add rax, r14
    inc rax
    mov r15, rax                ; d2
    ; no third dot
    lea rdi, [r12+r15+1]
    mov rsi, r13
    sub rsi, r15
    dec rsi
    mov edx, '.'
    call find_dot
    cmp rax, -1
    jne .invalid
    ; segment lengths
    test r14, r14
    jz .invalid
    mov rbx, r15
    sub rbx, r14
    dec rbx                     ; payload segment length
    test rbx, rbx
    jz .invalid
    mov rax, r13
    sub rax, r15
    dec rax                     ; signature segment length
    test rax, rax
    jz .invalid
    mov [rbp-112], rax          ; sig segment length
    ; header decode
    mov rdi, r12
    mov rsi, r14
    lea rdx, [rsp+JV_HDR]
    mov ecx, 4096
    lea r8, [rbp-104]
    call b64url_decode
    cmp eax, -1
    je .invalid
    ; alg
    lea rdi, [rsp+JV_HDR]
    mov esi, [rbp-104]
    lea rdx, [rsp+JV_ALG]
    mov ecx, 32
    call json_parse_alg
    cmp rax, 5
    jne .invalid
    lea rdi, [rsp+JV_ALG]
    mov rsi, 5
    lea rdx, [rel jwt_hs256]
    mov rcx, 5
    call mem_eq
    test eax, eax
    jz .invalid
    ; signature decode
    lea rdi, [r12+r15+1]
    mov rsi, [rbp-112]
    lea rdx, [rsp+JV_SIG]
    mov ecx, 32
    lea r8, [rbp-104]
    call b64url_decode
    cmp eax, -1
    je .invalid
    cmp qword [rbp-104], 32
    jne .invalid
    ; payload decode
    lea rdi, [r12+r14+1]
    mov rsi, rbx
    lea rdx, [rsp+JV_PAY]
    mov ecx, 8192
    lea r8, [rbp-104]
    call b64url_decode
    cmp eax, -1
    je .invalid
    ; HMAC over "<header>.<payload>"
    mov rdi, [rbp-56]
    mov rsi, [rbp-64]
    mov rdx, r12
    mov rcx, r14
    lea r8, [rel jwt_dot]
    mov r9, 1
    lea rax, [rsp+JV_SIG]
    push rax                     ; alignment pad (arg 10)
    push rax                     ; arg 9: expected signature
    push rbx                     ; arg 8: m3len
    lea rax, [r12+r14+1]
    push rax                     ; arg 7: m3
    call hmac_verify
    add rsp, 32
    test eax, eax
    jz .invalid
    ; payload
    lea rdi, [rsp+JV_PAY]
    mov esi, [rbp-104]
    mov rdx, [rbp-88]
    lea rcx, [rsp+JV_SUB]
    mov r8, [rbp-96]
    call json_parse_payload
    test eax, eax
    jnz .invalid
    mov rbx, [rbp-88]
    ; exp present and in the future
    test qword [rbx+J_FLAGS], JF_EXP
    jz .invalid
    mov eax, 228                ; clock_gettime
    xor edi, edi                ; CLOCK_REALTIME
    lea rsi, [rbp-128]
    syscall
    mov rax, [rbp-128]
    cmp [rbx+J_EXP], rax
    jbe .invalid
    ; sub and username present and well typed
    test qword [rbx+J_FLAGS], JF_SUB
    jz .bad_payload
    test qword [rbx+J_FLAGS], JF_UNAME
    jz .bad_payload
    mov rax, [rbx+J_SUBLEN]
    test rax, rax
    jz .bad_payload
    lea rdi, [rsp+JV_SUB]
    mov rsi, rax
    call u64_parse
    cmp rax, -1
    je .bad_payload
    test rax, rax
    jz .bad_payload
    mov [rbx+J_UID], rax
    xor eax, eax
    jmp .done
.invalid:
    mov eax, 2
    jmp .done
.bad_payload:
    mov eax, 3
.done:
    add rsp, 16408
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; find_dot(rdi=ptr, rsi=len, edx='.') -> rax = index or -1
; local copy to avoid a PLT call for a hot path
; -----------------------------------------------------------------------------
find_dot:
    xor rcx, rcx
.loop:
    cmp rcx, rsi
    jae .no
    cmp byte [rdi+rcx], dl
    je .yes
    inc rcx
    jmp .loop
.yes:
    mov rax, rcx
    ret
.no:
    mov rax, -1
    ret
