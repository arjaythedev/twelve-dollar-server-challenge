; JSON reader: validation, string decoding, and the three shapes this API
; needs (request body, JWT payload, JWT header alg).

%include "common.inc"

extern mem_eq
extern trim_ws
extern utf8_count

section .rodata
json_key_body:      db 'body'
json_key_alg:       db 'alg'
json_key_sub:       db 'sub'
json_key_exp:       db 'exp'
json_key_username:  db 'username'
lit_true:           db 'true'
lit_false:          db 'false'
lit_null:           db 'null'

section .text

global json_skip_ws
global json_scan_string
global json_skip_number
global json_skip_value
global json_decode_string
global json_parse_body
global json_parse_alg
global json_parse_payload

; -----------------------------------------------------------------------------
; json_skip_ws(rdi=ptr, rsi=end) -> rax = first non-whitespace
; -----------------------------------------------------------------------------
json_skip_ws:
    mov rax, rdi
.loop:
    cmp rax, rsi
    jae .done
    movzx ecx, byte [rax]
    cmp cl, 0x20
    je .adv
    cmp cl, 9
    je .adv
    cmp cl, 10
    je .adv
    cmp cl, 13
    je .adv
    jmp .done
.adv:
    inc rax
    jmp .loop
.done:
    ret

; -----------------------------------------------------------------------------
; json_scan_string(rdi=ptr at opening quote, rsi=end) -> rax = after quote, 0 bad
; -----------------------------------------------------------------------------
json_scan_string:
    cmp rdi, rsi
    jae .bad
    cmp byte [rdi], '"'
    jne .bad
    inc rdi
.loop:
    cmp rdi, rsi
    jae .bad
    movzx eax, byte [rdi]
    cmp al, '"'
    je .done
    cmp al, '\'
    je .esc
    cmp al, 0x20
    jb .bad
    inc rdi
    jmp .loop
.esc:
    inc rdi
    cmp rdi, rsi
    jae .bad
    movzx eax, byte [rdi]
    cmp al, 'u'
    je .uesc
    cmp al, '"'
    je .esc_ok
    cmp al, '\'
    je .esc_ok
    cmp al, '/'
    je .esc_ok
    cmp al, 'b'
    je .esc_ok
    cmp al, 'f'
    je .esc_ok
    cmp al, 'n'
    je .esc_ok
    cmp al, 'r'
    je .esc_ok
    cmp al, 't'
    je .esc_ok
    jmp .bad
.uesc:
    lea r8, [rdi+1]
    lea r9, [r8+4]
    cmp r9, rsi
    ja .bad
    mov ecx, 4
.uhex:
    movzx eax, byte [r8]
    cmp al, '0'
    jb .bad
    cmp al, '9'
    jbe .uhex_ok
    or al, 32
    cmp al, 'a'
    jb .bad
    cmp al, 'f'
    ja .bad
.uhex_ok:
    inc r8
    dec ecx
    jnz .uhex
    add rdi, 5
    jmp .loop
.esc_ok:
    inc rdi
    jmp .loop
.done:
    lea rax, [rdi+1]
    ret
.bad:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; json_skip_number(rdi=ptr, rsi=end) -> rax = after number, 0 bad
; -----------------------------------------------------------------------------
json_skip_number:
    mov rax, rdi
    cmp rax, rsi
    jae .bad
    cmp byte [rax], '-'
    jne .int
    inc rax
    cmp rax, rsi
    jae .bad
.int:
    cmp byte [rax], '0'
    jne .int_nonzero
    inc rax
    jmp .frac
.int_nonzero:
    cmp byte [rax], '1'
    jb .bad
    cmp byte [rax], '9'
    ja .bad
.digits:
    inc rax
    cmp rax, rsi
    jae .done
    movzx edx, byte [rax]
    cmp dl, '0'
    jb .frac
    cmp dl, '9'
    ja .frac
    jmp .digits
.frac:
    cmp rax, rsi
    jae .done
    cmp byte [rax], '.'
    jne .exp
    inc rax
    cmp rax, rsi
    jae .bad
    movzx edx, byte [rax]
    cmp dl, '0'
    jb .bad
    cmp dl, '9'
    ja .bad
.frac_digits:
    inc rax
    cmp rax, rsi
    jae .done
    movzx edx, byte [rax]
    cmp dl, '0'
    jb .exp
    cmp dl, '9'
    ja .exp
    jmp .frac_digits
.exp:
    cmp rax, rsi
    jae .done
    movzx edx, byte [rax]
    or dl, 32
    cmp dl, 'e'
    jne .done
    inc rax
    cmp rax, rsi
    jae .bad
    movzx edx, byte [rax]
    cmp dl, '+'
    je .exp_sign
    cmp dl, '-'
    jne .exp_digits_start
.exp_sign:
    inc rax
    cmp rax, rsi
    jae .bad
.exp_digits_start:
    movzx edx, byte [rax]
    cmp dl, '0'
    jb .bad
    cmp dl, '9'
    ja .bad
.exp_digits:
    inc rax
    cmp rax, rsi
    jae .done
    movzx edx, byte [rax]
    cmp dl, '0'
    jb .done
    cmp dl, '9'
    ja .done
    jmp .exp_digits
.done:
    ret
.bad:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; json_lit(rdi=ptr, rsi=end, rdx=literal, ecx=len) -> rax=after, 0 bad
; -----------------------------------------------------------------------------
json_lit:
    lea r8, [rdi+rcx]
    cmp r8, rsi
    ja .bad
    xor r9, r9
.loop:
    cmp r9, rcx
    jae .yes
    mov al, [rdi+r9]
    cmp al, [rdx+r9]
    jne .bad
    inc r9
    jmp .loop
.yes:
    mov rax, r8
    ret
.bad:
    xor eax, eax
    ret

; -----------------------------------------------------------------------------
; json_skip_value(rdi=ptr, rsi=end, edx=depth) -> rax=after, 0 bad
; -----------------------------------------------------------------------------
json_skip_value:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rsi
    mov r13d, edx
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    movzx eax, byte [r14]
    cmp al, '"'
    je .string
    cmp al, '{'
    je .object
    cmp al, '['
    je .array
    cmp al, '-'
    je .number
    cmp al, '0'
    jb .literal
    cmp al, '9'
    jbe .number
.literal:
    cmp al, 't'
    je .lt
    cmp al, 'f'
    je .lf
    cmp al, 'n'
    je .ln
    jmp .err
.lt:
    mov rdi, r14
    mov rsi, r12
    lea rdx, [rel lit_true]
    mov ecx, 4
    call json_lit
    jmp .result
.lf:
    mov rdi, r14
    mov rsi, r12
    lea rdx, [rel lit_false]
    mov ecx, 5
    call json_lit
    jmp .result
.ln:
    mov rdi, r14
    mov rsi, r12
    lea rdx, [rel lit_null]
    mov ecx, 4
    call json_lit
    jmp .result
.string:
    mov rdi, r14
    mov rsi, r12
    call json_scan_string
    jmp .result
.number:
    mov rdi, r14
    mov rsi, r12
    call json_skip_number
    jmp .result
.array:
    cmp r13d, 64
    jae .err
    inc r14
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    cmp byte [r14], ']'
    je .arr_empty
.arr_loop:
    mov rdi, r14
    mov rsi, r12
    lea edx, [r13+1]
    call json_skip_value
    test rax, rax
    jz .err
    mov r14, rax
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    mov al, [r14]
    cmp al, ','
    je .arr_comma
    cmp al, ']'
    je .arr_close
    jmp .err
.arr_comma:
    inc r14
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    jmp .arr_loop
.arr_empty:
    inc r14
    jmp .ok
.arr_close:
    inc r14
    jmp .ok
.object:
    cmp r13d, 64
    jae .err
    inc r14
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    cmp byte [r14], '}'
    je .obj_empty
.obj_loop:
    cmp byte [r14], '"'
    jne .err
    mov rdi, r14
    mov rsi, r12
    call json_scan_string
    test rax, rax
    jz .err
    mov r14, rax
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    cmp byte [r14], ':'
    jne .err
    inc r14
    mov rdi, r14
    mov rsi, r12
    lea edx, [r13+1]
    call json_skip_value
    test rax, rax
    jz .err
    mov r14, rax
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    mov al, [r14]
    cmp al, ','
    je .obj_comma
    cmp al, '}'
    je .obj_close
    jmp .err
.obj_comma:
    inc r14
    mov rdi, r14
    mov rsi, r12
    call json_skip_ws
    mov r14, rax
    cmp r14, r12
    jae .err
    jmp .obj_loop
.obj_empty:
    inc r14
    jmp .ok
.obj_close:
    inc r14
.ok:
    mov rax, r14
    jmp .done
.result:
    test rax, rax
    jz .err
    jmp .done
.err:
    xor eax, eax
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
; hex4(rdi=ptr) -> eax = 16-bit value of 4 hex chars, -1 on bad char
; -----------------------------------------------------------------------------
hex4:
    xor eax, eax
    xor r8, r8
    mov ecx, 4
.loop:
    shl eax, 4
    movzx edx, byte [rdi+r8]
    cmp dl, '0'
    jb .bad
    cmp dl, '9'
    jbe .dig
    or dl, 32
    cmp dl, 'a'
    jb .bad
    cmp dl, 'f'
    ja .bad
    sub dl, 'a'-10
    jmp .add
.dig:
    sub dl, '0'
.add:
    movzx edx, dl
    add eax, edx
    inc r8
    dec ecx
    jnz .loop
    ret
.bad:
    mov eax, -1
    ret

; -----------------------------------------------------------------------------
; json_decode_string(rdi=src at quote, rsi=end after quote, rdx=dst, rcx=cap)
;   -> rax = decoded length, or -1
; -----------------------------------------------------------------------------
json_decode_string:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    lea r13, [rsi-1]
    mov r14, rdx
    mov r15, rcx
    inc r12
    xor ebx, ebx
.loop:
    cmp r12, r13
    jae .done
    movzx eax, byte [r12]
    inc r12
    cmp al, '\'
    je .esc
    cmp rbx, r15
    jae .fail
    mov [r14+rbx], al
    inc rbx
    jmp .loop
.esc:
    cmp r12, r13
    jae .fail
    movzx eax, byte [r12]
    inc r12
    cmp al, '"'
    je .emit
    cmp al, '\'
    je .emit
    cmp al, '/'
    je .emit
    cmp al, 'b'
    je .emit_b
    cmp al, 'f'
    je .emit_f
    cmp al, 'n'
    je .emit_n
    cmp al, 'r'
    je .emit_r
    cmp al, 't'
    je .emit_t
    cmp al, 'u'
    je .uesc
    jmp .fail
.emit_b:
    mov al, 8
    jmp .emit
.emit_f:
    mov al, 12
    jmp .emit
.emit_n:
    mov al, 10
    jmp .emit
.emit_r:
    mov al, 13
    jmp .emit
.emit_t:
    mov al, 9
.emit:
    cmp rbx, r15
    jae .fail
    mov [r14+rbx], al
    inc rbx
    jmp .loop
.uesc:
    lea r8, [r12+4]
    cmp r8, r13
    ja .fail
    mov rdi, r12
    call hex4
    cmp eax, -1
    je .fail
    add r12, 4
    cmp eax, 0xd800
    jb .encode
    cmp eax, 0xdbff
    ja .encode
    mov r10d, eax
    lea r8, [r12+6]
    cmp r8, r13
    ja .fail
    cmp byte [r12], '\'
    jne .fail
    cmp byte [r12+1], 'u'
    jne .fail
    lea rdi, [r12+2]
    call hex4
    cmp eax, -1
    je .fail
    cmp eax, 0xdc00
    jb .fail
    cmp eax, 0xdfff
    ja .fail
    add r12, 6
    sub r10d, 0xd800
    shl r10d, 10
    sub eax, 0xdc00
    add eax, r10d
    add eax, 0x10000
.encode:
    cmp eax, 0x80
    jb .u1
    cmp eax, 0x800
    jb .u2
    cmp eax, 0x10000
    jb .u3
    lea r8, [rbx+4]
    cmp r8, r15
    ja .fail
    mov edx, eax
    mov r8d, eax
    shr r8d, 18
    or r8d, 0xf0
    mov [r14+rbx], r8b
    mov r8d, edx
    shr r8d, 12
    and r8d, 0x3f
    or r8d, 0x80
    mov [r14+rbx+1], r8b
    mov r8d, edx
    shr r8d, 6
    and r8d, 0x3f
    or r8d, 0x80
    mov [r14+rbx+2], r8b
    and edx, 0x3f
    or edx, 0x80
    mov [r14+rbx+3], dl
    add rbx, 4
    jmp .loop
.u3:
    lea r8, [rbx+3]
    cmp r8, r15
    ja .fail
    mov edx, eax
    mov r8d, eax
    shr r8d, 12
    or r8d, 0xe0
    mov [r14+rbx], r8b
    mov r8d, edx
    shr r8d, 6
    and r8d, 0x3f
    or r8d, 0x80
    mov [r14+rbx+1], r8b
    and edx, 0x3f
    or edx, 0x80
    mov [r14+rbx+2], dl
    add rbx, 3
    jmp .loop
.u2:
    lea r8, [rbx+2]
    cmp r8, r15
    ja .fail
    mov edx, eax
    shr eax, 6
    or eax, 0xc0
    mov [r14+rbx], al
    and edx, 0x3f
    or edx, 0x80
    mov [r14+rbx+1], dl
    add rbx, 2
    jmp .loop
.u1:
    cmp rbx, r15
    jae .fail
    mov [r14+rbx], al
    inc rbx
    jmp .loop
.done:
    mov rax, rbx
    jmp .out
.fail:
    mov rax, -1
.out:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; json_parse_body(rdi=ptr, rsi=len, rdx=dst, rcx=cap)
;   -> eax = 0 ok / 1 malformed / 2 no string body / 3 empty / 4 too long
;      rdx = trimmed body pointer, r8 = trimmed body length (status 0)
; -----------------------------------------------------------------------------
json_parse_body:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 56
    mov r12, rdi
    lea r13, [rdi+rsi]
    mov [rbp-64], rdx
    mov [rbp-72], rcx
    mov dword [rbp-80], 0
    mov dword [rbp-84], 0
    mov dword [rbp-88], 0
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    cmp byte [r12], '{'
    jne .malformed
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    cmp byte [r12], '}'
    je .empty_obj
.loop:
    cmp byte [r12], '"'
    jne .malformed
    mov [rbp-96], r12
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .malformed
    mov r14, rax
    mov r15, r14
    sub r15, [rbp-96]
    sub r15, 2
    mov r12, r14
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    cmp byte [r12], ':'
    jne .malformed
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    cmp r15, 4
    jne .other_value
    mov rdi, [rbp-96]
    inc rdi
    mov rsi, 4
    lea rdx, [rel json_key_body]
    mov rcx, 4
    call mem_eq
    test eax, eax
    jz .other_value
    mov dword [rbp-84], 1
    cmp byte [r12], '"'
    jne .body_not_string
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .malformed
    mov r14, rax
    mov rdi, r12
    mov rsi, r14
    mov rdx, [rbp-64]
    mov rcx, [rbp-72]
    call json_decode_string
    cmp rax, -1
    je .malformed
    mov [rbp-80], eax
    mov dword [rbp-88], 1
    mov r12, r14
    jmp .after_value
.body_not_string:
    mov dword [rbp-88], 0
    mov rdi, r12
    mov rsi, r13
    mov edx, 1
    call json_skip_value
    test rax, rax
    jz .malformed
    mov r12, rax
    jmp .after_value
.other_value:
    mov rdi, r12
    mov rsi, r13
    mov edx, 1
    call json_skip_value
    test rax, rax
    jz .malformed
    mov r12, rax
.after_value:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    mov al, [r12]
    cmp al, ','
    je .comma
    cmp al, '}'
    je .close
    jmp .malformed
.comma:
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .malformed
    jmp .loop
.empty_obj:
    inc r12
    jmp .finished
.close:
    inc r12
.finished:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    cmp rax, r13
    jne .malformed
    cmp dword [rbp-84], 0
    je .missing
    cmp dword [rbp-88], 0
    je .missing
    mov rdi, [rbp-64]
    mov esi, [rbp-80]
    call trim_ws
    test rdx, rdx
    jz .empty
    mov r14, rax
    mov r15, rdx
    mov rdi, rax
    mov rsi, rdx
    call utf8_count
    cmp rax, 500
    ja .toolong
    xor eax, eax
    mov rdx, r14
    mov r8, r15
    jmp .done
.malformed:
    mov eax, 1
    jmp .done
.missing:
    mov eax, 2
    jmp .done
.empty:
    mov eax, 3
    jmp .done
.toolong:
    mov eax, 4
.done:
    add rsp, 56
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; json_parse_alg(rdi=ptr, rsi=len, rdx=dst, rcx=cap)
;   -> rax = decoded alg length, -1 malformed JSON, -2 missing / not a string
; -----------------------------------------------------------------------------
json_parse_alg:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 64
    mov r12, rdi
    lea r13, [rdi+rsi]
    mov [rbp-64], rdx
    mov [rbp-72], rcx
    mov dword [rbp-80], 0
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], '{'
    jne .bad
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], '}'
    je .empty
.loop:
    cmp byte [r12], '"'
    jne .bad
    mov [rbp-88], r12
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .bad
    mov r14, rax
    mov r15, r14
    sub r15, [rbp-88]
    sub r15, 2
    mov r12, r14
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], ':'
    jne .bad
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp r15, 3
    jne .other
    mov rdi, [rbp-88]
    inc rdi
    mov rsi, 3
    lea rdx, [rel json_key_alg]
    mov rcx, 3
    call mem_eq
    test eax, eax
    jz .other
    cmp byte [r12], '"'
    jne .other
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .bad
    mov r14, rax
    mov rdi, r12
    mov rsi, r14
    mov rdx, [rbp-64]
    mov rcx, [rbp-72]
    call json_decode_string
    cmp rax, -1
    je .bad
    mov [rbp-80], eax
    mov dword [rbp-84], 1
    mov r12, r14
    jmp .after
.other:
    mov rdi, r12
    mov rsi, r13
    mov edx, 1
    call json_skip_value
    test rax, rax
    jz .bad
    mov r12, rax
.after:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    mov al, [r12]
    cmp al, ','
    je .comma
    cmp al, '}'
    je .close
    jmp .bad
.comma:
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    jmp .loop
.empty:
    inc r12
    jmp .finished
.close:
    inc r12
.finished:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    cmp rax, r13
    jne .bad
    cmp dword [rbp-84], 0
    je .missing
    mov eax, [rbp-80]
    jmp .done
.bad:
    mov rax, -1
    jmp .done
.missing:
    mov rax, -2
.done:
    add rsp, 64
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; json_parse_payload(rdi=ptr, rsi=len, rdx=out, rcx=sub_buf, r8=uname_buf)
;   -> eax = 0 valid JSON / -1 malformed.  Fills J_* fields and flags.
;   Fixed capacities: sub 32 bytes, username USER_SIZE bytes.
; -----------------------------------------------------------------------------
json_parse_payload:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 56
    mov r12, rdi
    lea r13, [rdi+rsi]
    mov [rbp-64], rdx
    mov [rbp-72], rcx
    mov [rbp-80], r8
    ; zero result
    mov qword [rdx+J_UID], 0
    mov qword [rdx+J_UNAMELEN], 0
    mov qword [rdx+J_EXP], 0
    mov qword [rdx+J_FLAGS], 0
    mov qword [rdx+J_SUBLEN], 0
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], '{'
    jne .bad
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], '}'
    je .empty_obj
.loop:
    cmp byte [r12], '"'
    jne .bad
    mov [rbp-88], r12
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .bad
    mov r14, rax
    mov r15, r14
    sub r15, [rbp-88]
    sub r15, 2
    mov r12, r14
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    cmp byte [r12], ':'
    jne .bad
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    ; sub
    cmp r15, 3
    jne .try_username
    mov rdi, [rbp-88]
    inc rdi
    mov rsi, 3
    lea rdx, [rel json_key_sub]
    mov rcx, 3
    call mem_eq
    test eax, eax
    jz .try_exp
    cmp byte [r12], '"'
    jne .skip_value
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .bad
    mov r14, rax
    mov rdi, r12
    mov rsi, r14
    mov rdx, [rbp-72]
    mov rcx, 32
    call json_decode_string
    cmp rax, -1
    je .bad
    mov rbx, [rbp-64]
    mov [rbx+J_SUBLEN], rax
    or qword [rbx+J_FLAGS], JF_SUB
    mov r12, r14
    jmp .after_value
.try_username:
    cmp r15, 8
    jne .try_exp
    mov rdi, [rbp-88]
    inc rdi
    mov rsi, 8
    lea rdx, [rel json_key_username]
    mov rcx, 8
    call mem_eq
    test eax, eax
    jz .try_exp
    cmp byte [r12], '"'
    jne .skip_value
    mov rdi, r12
    mov rsi, r13
    call json_scan_string
    test rax, rax
    jz .bad
    mov r14, rax
    mov rdi, r12
    mov rsi, r14
    mov rdx, [rbp-80]
    mov rcx, USER_SIZE
    call json_decode_string
    cmp rax, -1
    je .bad
    mov rbx, [rbp-64]
    mov [rbx+J_UNAMELEN], rax
    or qword [rbx+J_FLAGS], JF_UNAME
    mov r12, r14
    jmp .after_value
.try_exp:
    cmp r15, 3
    jne .skip_value
    mov rdi, [rbp-88]
    inc rdi
    mov rsi, 3
    lea rdx, [rel json_key_exp]
    mov rcx, 3
    call mem_eq
    test eax, eax
    jz .skip_value
    movzx eax, byte [r12]
    cmp al, '-'
    je .exp_num
    cmp al, '0'
    jb .skip_value
    cmp al, '9'
    ja .skip_value
.exp_num:
    mov rdi, r12
    mov rsi, r13
    call json_skip_number
    test rax, rax
    jz .bad
    mov r14, rax
    ; integer part -> u64 (negative becomes 0, i.e. expired)
    mov rdi, r12
    mov rsi, r14
    sub rsi, r12
    cmp byte [r12], '-'
    jne .exp_parse
    xor eax, eax
    jmp .exp_store
.exp_parse:
    ; stop at '.', 'e', 'E'
    xor ecx, ecx
.exp_digit_len:
    cmp rcx, rsi
    jae .exp_len_ok
    movzx edx, byte [rdi+rcx]
    cmp dl, '0'
    jb .exp_len_ok
    cmp dl, '9'
    ja .exp_len_ok
    inc rcx
    jmp .exp_digit_len
.exp_len_ok:
    test rcx, rcx
    jz .exp_zero
    mov rsi, rcx
    call u64_parse
    cmp rax, -1
    jne .exp_store
.exp_zero:
    xor eax, eax
.exp_store:
    mov rbx, [rbp-64]
    mov [rbx+J_EXP], rax
    or qword [rbx+J_FLAGS], JF_EXP
    mov r12, r14
    jmp .after_value
.skip_value:
    mov rdi, r12
    mov rsi, r13
    mov edx, 1
    call json_skip_value
    test rax, rax
    jz .bad
    mov r12, rax
.after_value:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    mov al, [r12]
    cmp al, ','
    je .comma
    cmp al, '}'
    je .close
    jmp .bad
.comma:
    inc r12
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    mov r12, rax
    cmp r12, r13
    jae .bad
    jmp .loop
.empty_obj:
    inc r12
    jmp .finished
.close:
    inc r12
.finished:
    mov rdi, r12
    mov rsi, r13
    call json_skip_ws
    cmp rax, r13
    jne .bad
    xor eax, eax
    jmp .done
.bad:
    mov eax, -1
.done:
    add rsp, 56
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

extern u64_parse
