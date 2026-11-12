; SHA-256, HMAC-SHA256 and base64url decoding.  Everything is self-contained:
; no OpenSSL, no libcrypto.

%include "common.inc"

extern mem_copy

section .rodata
sha256_k:
    dd 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    dd 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    dd 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    dd 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    dd 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    dd 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    dd 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    dd 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    dd 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    dd 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    dd 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    dd 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    dd 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    dd 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    dd 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    dd 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2

section .bss
global b64tab
b64tab: resb 256

section .text

global sha256_init
global sha256_update
global sha256_final
global hmac_verify
global b64url_init
global b64url_decode

%define SHA_TOTAL   32
%define SHA_BUFLEN  40
%define SHA_BUF     48

; -----------------------------------------------------------------------------
; sha256_init(rdi=ctx)
; -----------------------------------------------------------------------------
sha256_init:
    mov dword [rdi+0],  0x6a09e667
    mov dword [rdi+4],  0xbb67ae85
    mov dword [rdi+8],  0x3c6ef372
    mov dword [rdi+12], 0xa54ff53a
    mov dword [rdi+16], 0x510e527f
    mov dword [rdi+20], 0x9b05688c
    mov dword [rdi+24], 0x1f83d9ab
    mov dword [rdi+28], 0x5be0cd19
    mov qword [rdi+SHA_TOTAL], 0
    mov dword [rdi+SHA_BUFLEN], 0
    ret

; -----------------------------------------------------------------------------
; sha256_transform(rdi=ctx, rsi=block)
; -----------------------------------------------------------------------------
sha256_transform:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 288
    mov r12, rdi
    mov r13, rsi
    lea r14, [rel sha256_k]
    ; W[0..15]
    xor ecx, ecx
.load:
    cmp ecx, 16
    jae .expand
    mov eax, [r13+rcx*4]
    bswap eax
    mov [rsp+rcx*4], eax
    inc ecx
    jmp .load
.expand:
    cmp ecx, 64
    jae .vars
    mov eax, [rsp+rcx*4-60]
    mov edx, eax
    ror edx, 7
    mov ebx, eax
    ror ebx, 18
    xor edx, ebx
    shr eax, 3
    xor edx, eax
    mov eax, [rsp+rcx*4-8]
    mov ebx, eax
    ror ebx, 17
    mov esi, eax
    ror esi, 19
    xor ebx, esi
    shr eax, 10
    xor ebx, eax
    add edx, ebx
    add edx, [rsp+rcx*4-64]
    add edx, [rsp+rcx*4-28]
    mov [rsp+rcx*4], edx
    inc ecx
    jmp .expand
.vars:
    mov eax, [r12+0]
    mov [rsp+256+0], eax
    mov eax, [r12+4]
    mov [rsp+256+4], eax
    mov eax, [r12+8]
    mov [rsp+256+8], eax
    mov eax, [r12+12]
    mov [rsp+256+12], eax
    mov eax, [r12+16]
    mov [rsp+256+16], eax
    mov eax, [r12+20]
    mov [rsp+256+20], eax
    mov eax, [r12+24]
    mov [rsp+256+24], eax
    mov eax, [r12+28]
    mov [rsp+256+28], eax
    xor r15d, r15d
.round:
    ; T1 = h + S1(e) + ch(e,f,g) + K[i] + W[i]
    mov eax, [rsp+256+16]
    mov ecx, eax
    ror ecx, 6
    mov edx, eax
    ror edx, 11
    xor ecx, edx
    mov edx, eax
    ror edx, 25
    xor ecx, edx
    mov edx, [rsp+256+20]
    mov ebx, eax
    not ebx
    and edx, eax
    and ebx, [rsp+256+24]
    xor edx, ebx
    add ecx, edx
    add ecx, [rsp+256+28]
    add ecx, [r14+r15*4]
    add ecx, [rsp+r15*4]
    mov r8d, ecx
    ; T2 = S0(a) + maj(a,b,c)
    mov eax, [rsp+256+0]
    mov ecx, eax
    ror ecx, 2
    mov edx, eax
    ror edx, 13
    xor ecx, edx
    mov edx, eax
    ror edx, 22
    xor ecx, edx
    mov edx, [rsp+256+0]
    and edx, [rsp+256+4]
    mov ebx, [rsp+256+0]
    and ebx, [rsp+256+8]
    xor edx, ebx
    mov ebx, [rsp+256+4]
    and ebx, [rsp+256+8]
    xor edx, ebx
    add ecx, edx
    mov r9d, ecx
    mov r10d, [rsp+256+12]
    add r10d, r8d
    mov r11d, r8d
    add r11d, r9d
    mov eax, [rsp+256+24]
    mov [rsp+256+28], eax
    mov eax, [rsp+256+20]
    mov [rsp+256+24], eax
    mov eax, [rsp+256+16]
    mov [rsp+256+20], eax
    mov [rsp+256+16], r10d
    mov eax, [rsp+256+8]
    mov [rsp+256+12], eax
    mov eax, [rsp+256+4]
    mov [rsp+256+8], eax
    mov eax, [rsp+256+0]
    mov [rsp+256+4], eax
    mov [rsp+256+0], r11d
    inc r15d
    cmp r15d, 64
    jb .round
    mov eax, [rsp+256+0]
    add [r12+0], eax
    mov eax, [rsp+256+4]
    add [r12+4], eax
    mov eax, [rsp+256+8]
    add [r12+8], eax
    mov eax, [rsp+256+12]
    add [r12+12], eax
    mov eax, [rsp+256+16]
    add [r12+16], eax
    mov eax, [rsp+256+20]
    add [r12+20], eax
    mov eax, [rsp+256+24]
    add [r12+24], eax
    mov eax, [rsp+256+28]
    add [r12+28], eax
    add rsp, 288
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; sha256_update(rdi=ctx, rsi=ptr, rdx=len)
; -----------------------------------------------------------------------------
sha256_update:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    add [r12+SHA_TOTAL], r14
    mov eax, [r12+SHA_BUFLEN]
    test eax, eax
    jz .blocks
    mov ecx, 64
    sub ecx, eax
    mov r15, r14
    cmp r15, rcx
    cmova r15, rcx
    lea rdi, [r12+SHA_BUF]
    add rdi, rax
    mov rsi, r13
    mov rdx, r15
    call mem_copy
    add [r12+SHA_BUFLEN], r15d
    add r13, r15
    sub r14, r15
    cmp dword [r12+SHA_BUFLEN], 64
    jne .blocks
    mov rdi, r12
    lea rsi, [r12+SHA_BUF]
    call sha256_transform
    mov dword [r12+SHA_BUFLEN], 0
.blocks:
    cmp r14, 64
    jb .tail
    mov rdi, r12
    mov rsi, r13
    call sha256_transform
    add r13, 64
    sub r14, 64
    jmp .blocks
.tail:
    test r14, r14
    jz .done
    lea rdi, [r12+SHA_BUF]
    mov rsi, r13
    mov rdx, r14
    call mem_copy
    mov [r12+SHA_BUFLEN], r14d
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; sha256_final(rdi=ctx, rsi=out)
; -----------------------------------------------------------------------------
sha256_final:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    mov r14, [r12+SHA_TOTAL]
    shl r14, 3
    mov eax, [r12+SHA_BUFLEN]
    lea rbx, [r12+SHA_BUF]
    mov byte [rbx+rax], 0x80
    inc eax
    mov [r12+SHA_BUFLEN], eax
    cmp eax, 56
    jbe .pad
    ; zero to 64
    mov ecx, eax
.zero1:
    cmp ecx, 64
    jae .do1
    mov byte [rbx+rcx], 0
    inc ecx
    jmp .zero1
.do1:
    mov rdi, r12
    mov rsi, rbx
    call sha256_transform
    mov dword [r12+SHA_BUFLEN], 0
    xor eax, eax
.pad:
    mov ecx, eax
.zero2:
    cmp ecx, 56
    jae .lenfield
    mov byte [rbx+rcx], 0
    inc ecx
    jmp .zero2
.lenfield:
    mov rax, r14
    bswap rax
    mov [rbx+56], rax
    mov rdi, r12
    mov rsi, rbx
    call sha256_transform
    ; output big-endian
    xor ecx, ecx
.out:
    cmp ecx, 8
    jae .done
    mov eax, [r12+rcx*4]
    bswap eax
    mov [r13+rcx*4], eax
    inc ecx
    jmp .out
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; hmac_verify(key,keylen, m1,m1len, m2,m2len, m3,m3len, sig) -> eax=1 match
;   args 7..9 on the stack: [rbp+16]=m3, [rbp+24]=m3len, [rbp+32]=sig
; -----------------------------------------------------------------------------
hmac_verify:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 424
    ; locals live above the 328-byte scratch region, below the saved regs
    mov [rbp-136], rdi         ; key
    mov [rbp-128], rsi         ; keylen
    mov [rbp-120], rdx         ; m1
    mov [rbp-112], rcx         ; m1len
    mov [rbp-104], r8          ; m2
    mov [rbp-96], r9           ; m2len
    mov rax, [rbp+16]
    mov [rbp-88], rax          ; m3
    mov rax, [rbp+24]
    mov [rbp-80], rax          ; m3len
    mov rax, [rbp+32]
    mov [rbp-72], rax          ; sig
    ; zero pads
    lea rdi, [rsp]
    xor eax, eax
    mov ecx, 128
    rep stosb
    ; key block
    mov rsi, [rbp-136]
    mov rcx, [rbp-128]
    cmp rcx, 64
    jbe .key_copy
    lea rdi, [rsp+128]
    call sha256_init
    lea rdi, [rsp+128]
    mov rsi, [rbp-136]
    mov rdx, [rbp-128]
    call sha256_update
    lea rdi, [rsp+128]
    lea rsi, [rsp+240]
    call sha256_final
    lea rsi, [rsp+240]
    mov ecx, 32
.key_copy:
    test rcx, rcx
    jz .pads
    xor edx, edx
.copy_key:
    mov al, [rsi+rdx]
    mov [rsp+rdx], al
    mov [rsp+64+rdx], al
    inc rdx
    cmp rdx, rcx
    jb .copy_key
.pads:
    xor ecx, ecx
.pad_loop:
    cmp ecx, 64
    jae .inner
    mov al, [rsp+rcx]
    xor al, 0x36
    mov [rsp+rcx], al
    mov al, [rsp+64+rcx]
    xor al, 0x5c
    mov [rsp+64+rcx], al
    inc ecx
    jmp .pad_loop
.inner:
    lea rdi, [rsp+128]
    call sha256_init
    lea rdi, [rsp+128]
    lea rsi, [rsp]
    mov edx, 64
    call sha256_update
    lea rdi, [rsp+128]
    mov rsi, [rbp-120]
    mov rdx, [rbp-112]
    call sha256_update
    lea rdi, [rsp+128]
    mov rsi, [rbp-104]
    mov rdx, [rbp-96]
    call sha256_update
    lea rdi, [rsp+128]
    mov rsi, [rbp-88]
    mov rdx, [rbp-80]
    call sha256_update
    lea rdi, [rsp+128]
    lea rsi, [rsp+240]
    call sha256_final
.outer:
    lea rdi, [rsp+128]
    call sha256_init
    lea rdi, [rsp+128]
    lea rsi, [rsp+64]
    mov edx, 64
    call sha256_update
    lea rdi, [rsp+128]
    lea rsi, [rsp+240]
    mov edx, 32
    call sha256_update
    lea rdi, [rsp+128]
    lea rsi, [rsp+272]
    call sha256_final
    ; constant-time compare
    mov rsi, [rbp-72]
    xor eax, eax
    xor ecx, ecx
.cmp:
    cmp ecx, 32
    jae .cmp_done
    mov dl, [rsp+272+rcx]
    xor dl, [rsi+rcx]
    or al, dl
    inc ecx
    jmp .cmp
.cmp_done:
    test al, al
    setz al
    movzx eax, al
    add rsp, 424
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; b64url_init()
; -----------------------------------------------------------------------------
b64url_init:
    lea rdi, [rel b64tab]
    mov al, 0xff
    mov ecx, 256
    rep stosb
    lea rdi, [rel b64tab+65]
    xor eax, eax
    mov ecx, 26
.loop_upper:
    mov [rdi], al
    inc rdi
    inc eax
    dec ecx
    jnz .loop_upper
    lea rdi, [rel b64tab+97]
    mov eax, 26
    mov ecx, 26
.loop_lower:
    mov [rdi], al
    inc rdi
    inc eax
    dec ecx
    jnz .loop_lower
    lea rdi, [rel b64tab+48]
    mov eax, 52
    mov ecx, 10
.loop_digit:
    mov [rdi], al
    inc rdi
    inc eax
    dec ecx
    jnz .loop_digit
    mov byte [rel b64tab+45], 62
    mov byte [rel b64tab+95], 63
    mov byte [rel b64tab+43], 62
    mov byte [rel b64tab+47], 63
    ret

; -----------------------------------------------------------------------------
; b64url_decode(rdi=src, rsi=len, rdx=dst, rcx=cap, r8=outlen*)
;   -> eax = 0 ok, -1 invalid / no room
; -----------------------------------------------------------------------------
b64url_decode:
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
    mov r14, rdx
    mov r15, rcx
    mov [rbp-56], r8
    lea rdi, [rel b64tab]
    xor ebx, ebx
    ; length % 4 == 1 is impossible
    mov rax, r13
    and eax, 3
    cmp eax, 1
    je .fail
.loop4:
    cmp r13, 4
    jb .tail
    movzx r8d, byte [r12]
    movzx eax, byte [rdi+r8]
    movzx r9d, byte [r12+1]
    movzx ecx, byte [rdi+r9]
    movzx r10d, byte [r12+2]
    movzx edx, byte [rdi+r10]
    movzx r11d, byte [r12+3]
    movzx esi, byte [rdi+r11]
    cmp eax, 0xff
    je .fail
    cmp ecx, 0xff
    je .fail
    cmp byte [r12+2], '='
    je .pad2
    cmp edx, 0xff
    je .fail
    cmp byte [r12+3], '='
    je .pad1
    cmp esi, 0xff
    je .fail
    ; three bytes
    lea r8, [rbx+3]
    cmp r8, r15
    ja .fail
    shl eax, 2
    mov r8d, ecx
    shr r8d, 4
    or eax, r8d
    mov [r14+rbx], al
    mov eax, ecx
    shl eax, 4
    mov r8d, edx
    shr r8d, 2
    or eax, r8d
    mov [r14+rbx+1], al
    mov eax, edx
    shl eax, 6
    or eax, esi
    mov [r14+rbx+2], al
    add rbx, 3
    add r12, 4
    sub r13, 4
    jmp .loop4
.pad2:
    cmp byte [r12+3], '='
    jne .fail
    lea r8, [rbx+1]
    cmp r8, r15
    ja .fail
    shl eax, 2
    mov r8d, ecx
    shr r8d, 4
    or eax, r8d
    mov [r14+rbx], al
    inc rbx
    xor r13d, r13d
    jmp .done
.pad1:
    lea r8, [rbx+2]
    cmp r8, r15
    ja .fail
    shl eax, 2
    mov r8d, ecx
    shr r8d, 4
    or eax, r8d
    mov [r14+rbx], al
    mov eax, ecx
    shl eax, 4
    mov r8d, edx
    shr r8d, 2
    or eax, r8d
    mov [r14+rbx+1], al
    add rbx, 2
    xor r13d, r13d
    jmp .done
.tail:
    cmp r13, 2
    je .tail2
    cmp r13, 3
    je .tail3
    test r13, r13
    jnz .fail
    jmp .done
.tail2:
    movzx r8d, byte [r12]
    movzx eax, byte [rdi+r8]
    movzx r9d, byte [r12+1]
    movzx ecx, byte [rdi+r9]
    cmp eax, 0xff
    je .fail
    cmp ecx, 0xff
    je .fail
    cmp rbx, r15
    jae .fail
    shl eax, 2
    mov r8d, ecx
    shr r8d, 4
    or eax, r8d
    mov [r14+rbx], al
    inc rbx
    jmp .done
.tail3:
    movzx r8d, byte [r12]
    movzx eax, byte [rdi+r8]
    movzx r9d, byte [r12+1]
    movzx ecx, byte [rdi+r9]
    movzx r10d, byte [r12+2]
    movzx edx, byte [rdi+r10]
    cmp eax, 0xff
    je .fail
    cmp ecx, 0xff
    je .fail
    cmp edx, 0xff
    je .fail
    lea r8, [rbx+2]
    cmp r8, r15
    ja .fail
    shl eax, 2
    mov r8d, ecx
    shr r8d, 4
    or eax, r8d
    mov [r14+rbx], al
    mov eax, ecx
    shl eax, 4
    mov r8d, edx
    shr r8d, 2
    or eax, r8d
    mov [r14+rbx+1], al
    add rbx, 2
.done:
    mov rax, [rbp-56]
    mov [rax], rbx
    xor eax, eax
    jmp .out
.fail:
    mov eax, -1
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
