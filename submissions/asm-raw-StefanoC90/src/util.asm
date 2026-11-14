; Small string / number / JSON-output helpers shared by the server.

%include "common.inc"

section .rodata
hexdigits: db '0123456789abcdef'
align 16
v_quote:  times 16 db '"'
v_bslash: times 16 db '\'
v_ctl:    times 16 db 0x1f
align 16
v_nl:     times 16 db 10

section .text

global append_bytes
global mem_copy
global append_u64
global append_json_string
global ci_eq
global starts_with
global mem_eq
global find_byte
global find_header_end
global trim_ws
global utf8_count
global u64_parse

; -----------------------------------------------------------------------------
; append_bytes(rdi=dst, rsi=src, rdx=len) -> rax = dst+len
; rep movsb has a fixed startup cost, so short copies (most of them here: the
; JSON keys and response header fragments) use a plain byte loop instead.
; -----------------------------------------------------------------------------
append_bytes:
    mov rax, rdi
    test rdx, rdx
    jz .done
    cmp rdx, 64
    jae .bulk
    mov rcx, rdx
.copy:
    mov r8b, [rsi]
    mov [rdi], r8b
    inc rsi
    inc rdi
    dec rcx
    jnz .copy
    mov rax, rdi
    ret
.bulk:
    mov rcx, rdx
    rep movsb
    mov rax, rdi
.done:
    ret

; -----------------------------------------------------------------------------
; mem_copy(rdi=dst, rsi=src, rdx=len)
; -----------------------------------------------------------------------------
mem_copy:
    mov rcx, rdx
    rep movsb
    ret

; -----------------------------------------------------------------------------
; append_u64(rdi=dst, rsi=value) -> rax = new end
; Divides by 10 with a multiply (no div in the loop, ~15 cycles per digit).
; -----------------------------------------------------------------------------
append_u64:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    mov rax, rsi
    lea r8, [rsp+32]
    mov r10, 0xCCCCCCCCCCCCCCCD
    xor ecx, ecx
.loop:
    mov r11, rax
    mul r10                     ; rdx = high(value * magic)
    mov rax, rdx
    shr rax, 3                  ; rax = value / 10
    lea rdx, [rax+rax*4]
    add rdx, rdx                ; rdx = (value/10) * 10
    sub r11, rdx                ; r11 = value % 10
    add r11b, '0'
    dec r8
    mov [r8], r11b
    inc ecx
    test rax, rax
    jnz .loop
    mov rsi, r8
    mov rdx, rcx
    call append_bytes
    leave
    ret

; -----------------------------------------------------------------------------
; append_json_string(rdi=dst, rsi=str, rdx=len) -> rax = new end
; Escapes quote, backslash and control bytes; everything else is copied raw.
; Scans 16 bytes at a time with SSE2 and copies the whole string in one go when
; nothing needs escaping (the normal case), so long bodies are not walked byte
; by byte. Short strings skip the scan: rep movsb would not pay for it.
; -----------------------------------------------------------------------------
append_json_string:
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
    lea r14, [rsi+rdx]
    mov byte [r12], '"'
    inc r12
    cmp rdx, 32
    jb .loop                    ; too short to be worth scanning
    movdqa xmm3, [rel v_quote]
    movdqa xmm4, [rel v_bslash]
    movdqa xmm5, [rel v_ctl]
    mov rbx, r13
.scan16:
    lea rcx, [rbx+16]
    cmp rcx, r14
    ja .scan_tail
    movdqu xmm0, [rbx]
    movdqa xmm1, xmm0
    pcmpeqb xmm1, xmm3
    movdqa xmm2, xmm0
    pcmpeqb xmm2, xmm4
    por xmm1, xmm2
    movdqa xmm2, xmm0
    pminub xmm2, xmm5
    pcmpeqb xmm2, xmm0
    por xmm1, xmm2
    pmovmskb eax, xmm1
    test eax, eax
    jnz .loop                   ; something needs escaping: byte loop
    add rbx, 16
    jmp .scan16
.scan_tail:
    cmp rbx, r14
    jae .clean
    movzx eax, byte [rbx]
    cmp al, '"'
    je .loop
    cmp al, '\'
    je .loop
    cmp al, 0x20
    jb .loop
    inc rbx
    jmp .scan_tail
.clean:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r14
    sub rdx, r13
    call append_bytes
    mov r12, rax
    jmp .close
.loop:
    cmp r13, r14
    jae .close
    movzx eax, byte [r13]
    inc r13
    cmp al, '"'
    je .quote
    cmp al, '\'
    je .backslash
    cmp al, 0x20
    jb .control
    mov [r12], al
    inc r12
    jmp .loop
.quote:
    mov byte [r12], '\'
    mov byte [r12+1], '"'
    add r12, 2
    jmp .loop
.backslash:
    mov byte [r12], '\'
    mov byte [r12+1], '\'
    add r12, 2
    jmp .loop
.control:
    mov byte [r12], '\'
    mov byte [r12+1], 'u'
    mov byte [r12+2], '0'
    mov byte [r12+3], '0'
    mov edx, eax
    shr eax, 4
    and eax, 0xf
    lea rbx, [rel hexdigits]
    mov al, [rbx+rax]
    mov [r12+4], al
    mov eax, edx
    and eax, 0xf
    mov al, [rbx+rax]
    mov [r12+5], al
    add r12, 6
    jmp .loop
.close:
    mov byte [r12], '"'
    inc r12
    mov rax, r12
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; ci_eq(rdi=a, rsi=alen, rdx=b, rcx=blen) -> eax = 1 equal ignoring ASCII case
; -----------------------------------------------------------------------------
ci_eq:
    cmp rsi, rcx
    jne .no
    xor r8, r8
.loop:
    cmp r8, rsi
    jae .yes
    movzx eax, byte [rdi+r8]
    movzx r9d, byte [rdx+r8]
    mov r10d, eax
    sub r10d, 'A'
    cmp r10d, 25
    ja .a_done
    add eax, 32
.a_done:
    mov r10d, r9d
    sub r10d, 'A'
    cmp r10d, 25
    ja .b_done
    add r9d, 32
.b_done:
    cmp eax, r9d
    jne .no
    inc r8
    jmp .loop
.yes:
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; starts_with(rdi=a, rsi=alen, rdx=b, rcx=blen) -> eax = 1 if a begins with b
; -----------------------------------------------------------------------------
starts_with:
    cmp rsi, rcx
    jb .no
    xor r8, r8
.loop:
    cmp r8, rcx
    jae .yes
    mov al, [rdi+r8]
    cmp al, [rdx+r8]
    jne .no
    inc r8
    jmp .loop
.yes:
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; mem_eq(rdi=a, rsi=alen, rdx=b, rcx=blen) -> eax = 1 equal
; -----------------------------------------------------------------------------
mem_eq:
    cmp rsi, rcx
    jne .no
    xor r8, r8
.loop:
    cmp r8, rsi
    jae .yes
    mov al, [rdi+r8]
    cmp al, [rdx+r8]
    jne .no
    inc r8
    jmp .loop
.yes:
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; find_byte(rdi=ptr, rsi=len, edx=byte) -> rax = index or -1
; -----------------------------------------------------------------------------
find_byte:
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

; -----------------------------------------------------------------------------
; find_header_end(rdi=ptr, rsi=len) -> rax = offset of CRLFCRLF or -1
; Finds CRLFCRLF by locating each LF 16 bytes at a time (SSE2) and checking the
; three bytes before it, so the common ~200-byte header is scanned in a dozen
; steps instead of ~800 byte comparisons.
; -----------------------------------------------------------------------------
find_header_end:
    cmp rsi, 4
    jb .no
    movdqa xmm3, [rel v_nl]
    xor ecx, ecx
.scan16:
    lea rdx, [rcx+16]
    cmp rdx, rsi
    ja .tail
    movdqu xmm0, [rdi+rcx]
    pcmpeqb xmm0, xmm3
    pmovmskb r9d, xmm0
    test r9d, r9d
    jz .next16
.bits:
    bsf r10d, r9d
    lea r11, [rcx+r10]
    cmp r11, 3
    jb .drop
    cmp byte [rdi+r11-1], 13
    jne .drop
    cmp byte [rdi+r11-2], 10
    jne .drop
    cmp byte [rdi+r11-3], 13
    jne .drop
    lea rax, [r11-3]
    ret
.drop:
    mov r10d, r9d
    dec r10d
    and r9d, r10d
    jnz .bits
.next16:
    add rcx, 16
    jmp .scan16
.tail:
    ; A CRLFCRLF whose LF sits in the tail may have started up to three
    ; bytes before the last scanned block, so rewind before the byte loop.
    cmp rcx, 3
    jb .tail_ready
    sub rcx, 3
.tail_ready:
    mov r8, rsi
    sub r8, 3
.tail_loop:
    cmp rcx, r8
    jae .no
    cmp byte [rdi+rcx], 13
    jne .tail_next
    cmp byte [rdi+rcx+1], 10
    jne .tail_next
    cmp byte [rdi+rcx+2], 13
    jne .tail_next
    cmp byte [rdi+rcx+3], 10
    je .yes
.tail_next:
    inc rcx
    jmp .tail_loop
.yes:
    mov rax, rcx
    ret
.no:
    mov rax, -1
    ret

; -----------------------------------------------------------------------------
; trim_ws(rdi=ptr, rsi=len) -> rax = ptr, rdx = len
; ASCII whitespace only.
; -----------------------------------------------------------------------------
trim_ws:
    mov rax, rdi
    mov rdx, rsi
.lead:
    test rdx, rdx
    jz .done
    movzx ecx, byte [rax]
    cmp cl, 0x20
    je .lead_adv
    cmp cl, 9
    je .lead_adv
    cmp cl, 10
    je .lead_adv
    cmp cl, 13
    je .lead_adv
    cmp cl, 11
    je .lead_adv
    cmp cl, 12
    je .lead_adv
    jmp .tail
.lead_adv:
    inc rax
    dec rdx
    jmp .lead
.tail:
    test rdx, rdx
    jz .done
    movzx ecx, byte [rax+rdx-1]
    cmp cl, 0x20
    je .tail_adv
    cmp cl, 9
    je .tail_adv
    cmp cl, 10
    je .tail_adv
    cmp cl, 13
    je .tail_adv
    cmp cl, 11
    je .tail_adv
    cmp cl, 12
    je .tail_adv
    jmp .done
.tail_adv:
    dec rdx
    jmp .tail
.done:
    ret

; -----------------------------------------------------------------------------
; utf8_count(rdi=ptr, rsi=len) -> rax = number of UTF-8 code points
; -----------------------------------------------------------------------------
utf8_count:
    xor rax, rax
    xor rcx, rcx
.loop:
    cmp rcx, rsi
    jae .done
    movzx edx, byte [rdi+rcx]
    and edx, 0xc0
    cmp edx, 0x80
    je .skip
    inc rax
.skip:
    inc rcx
    jmp .loop
.done:
    ret

; -----------------------------------------------------------------------------
; u64_parse(rdi=ptr, rsi=len) -> rax = value, or -1 if not all digits / overflow
; -----------------------------------------------------------------------------
u64_parse:
    test rsi, rsi
    jz .bad
    xor rax, rax
    xor rcx, rcx
.loop:
    cmp rcx, rsi
    jae .ok
    movzx r8d, byte [rdi+rcx]
    sub r8d, '0'
    cmp r8d, 9
    ja .bad
    mov r9, 10
    mul r9
    test rdx, rdx
    jnz .bad
    add rax, r8
    jc .bad
    inc rcx
    jmp .loop
.ok:
    ret
.bad:
    mov rax, -1
    ret
