; jwt.asm - HS256 JWT validation and decoding in x86_64 NASM
default rel

%include "common.inc"

extern EVP_sha256
extern HMAC
extern time
extern str_to_u64

section .rodata

; Base64URL character table for encoding (64 chars)
b64url_chars:
    db "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

; Base64URL decode table (256 bytes)
; 0xFF indicates invalid character
b64url_decode_table:
    times 45 db 0xFF        ; 0 - 44
    db 62                   ; 45 = '-'
    times 2 db 0xFF         ; 46 - 47
    db 52,53,54,55,56,57,58,59,60,61 ; 48 - 57 = '0'-'9'
    times 7 db 0xFF         ; 58 - 64
    ; 65 - 90 = 'A'-'Z' (0 - 25)
    db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25
    times 4 db 0xFF         ; 91 - 94
    db 63                   ; 95 = '_'
    db 0xFF                 ; 96
    ; 97 - 122 = 'a'-'z' (26 - 51)
    db 26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51
    times 133 db 0xFF       ; 123 - 255

section .text

global init_jwt
global jwt_verify
global b64url_encode_32
global b64url_decode

; =============================================================================
; init_jwt()
; Resolves and caches EVP_sha256() once at boot
; =============================================================================
init_jwt:
    sub rsp, 8
    call EVP_sha256
    mov [evp_sha256_md], rax
    add rsp, 8
    ret

; =============================================================================
; b64url_encode_32(const uint8_t *in_32 [rdi], char *out_43 [rsi])
; Encodes exactly 32 bytes of SHA256 output into 43 Base64URL characters
; (30 bytes -> 10 groups of 3 => 40 chars, 2 bytes -> 3 chars = 43 chars total)
; =============================================================================
b64url_encode_32:
    push rbx
    push r12
    push r13
    push r14

    mov r8, rdi             ; in
    mov r9, rsi             ; out
    lea r10, [b64url_chars]

    ; 10 groups of 3 bytes
    xor ecx, ecx            ; in index (0..30)
    xor edx, edx            ; out index (0..40)

.loop_30:
    cmp ecx, 30
    jge .last_2

    movzx eax, byte [r8 + rcx]
    shl eax, 8
    movzx ebx, byte [r8 + rcx + 1]
    or eax, ebx
    shl eax, 8
    movzx ebx, byte [r8 + rcx + 2]
    or eax, ebx             ; eax = (b0 << 16) | (b1 << 8) | b2

    ; char 0: (eax >> 18) & 0x3F
    mov ebx, eax
    shr ebx, 18
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx], bl

    ; char 1: (eax >> 12) & 0x3F
    mov ebx, eax
    shr ebx, 12
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx + 1], bl

    ; char 2: (eax >> 6) & 0x3F
    mov ebx, eax
    shr ebx, 6
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx + 2], bl

    ; char 3: eax & 0x3F
    mov ebx, eax
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx + 3], bl

    add rcx, 3
    add rdx, 4
    jmp .loop_30

.last_2:
    ; 2 remaining bytes: b30, b31
    movzx eax, byte [r8 + 30]
    shl eax, 8
    movzx ebx, byte [r8 + 31]
    or eax, ebx             ; 16 bits: eax = (b30 << 8) | b31
    shl eax, 2              ; 18 bits

    ; char 40: (eax >> 12) & 0x3F
    mov ebx, eax
    shr ebx, 12
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx], bl

    ; char 41: (eax >> 6) & 0x3F
    mov ebx, eax
    shr ebx, 6
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx + 1], bl

    ; char 42: eax & 0x3F
    mov ebx, eax
    and ebx, 0x3F
    mov bl, [r10 + rbx]
    mov [r9 + rdx + 2], bl

    mov byte [r9 + rdx + 3], 0  ; null terminate

    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; b64url_decode(const char *in [rdi], size_t in_len [rsi],
;               uint8_t *out [rdx], size_t *out_len [rcx])
; Returns: rax = 1 on success, 0 on invalid base64url
; =============================================================================
b64url_decode:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r8, rdi             ; in
    mov r9, rsi             ; in_len
    mov r10, rdx            ; out
    mov r11, rcx            ; out_len ptr
    lea r12, [b64url_decode_table]

    xor ecx, ecx            ; in_idx
    xor edx, edx            ; out_idx

.loop_4:
    mov rax, r9
    sub rax, rcx
    cmp rax, 4
    jl .remainder

    ; Load 4 characters
    movzx eax, byte [r8 + rcx]
    movzx ebx, byte [r12 + rax]
    cmp bl, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 1]
    movzx r13d, byte [r12 + rax]
    cmp r13b, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 2]
    movzx r14d, byte [r12 + rax]
    cmp r14b, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 3]
    movzx r15d, byte [r12 + rax]
    cmp r15b, 0xFF
    je .fail

    ; 24-bit val = (ebx << 18) | (r13d << 12) | (r14d << 6) | r15d
    shl ebx, 18
    shl r13d, 12
    shl r14d, 6
    or ebx, r13d
    or ebx, r14d
    or ebx, r15d

    ; byte 0
    mov eax, ebx
    shr eax, 16
    mov [r10 + rdx], al

    ; byte 1
    mov eax, ebx
    shr eax, 8
    mov [r10 + rdx + 1], al

    ; byte 2
    mov [r10 + rdx + 2], bl

    add rcx, 4
    add rdx, 3
    jmp .loop_4

.remainder:
    mov rax, r9
    sub rax, rcx
    test rax, rax
    jz .success
    cmp rax, 1
    je .fail                ; single base64 char is impossible in valid base64
    cmp rax, 2
    je .rem_2
    cmp rax, 3
    je .rem_3

.rem_2:
    movzx eax, byte [r8 + rcx]
    movzx ebx, byte [r12 + rax]
    cmp bl, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 1]
    movzx r13d, byte [r12 + rax]
    cmp r13b, 0xFF
    je .fail

    ; 1 byte output: (ebx << 2) | (r13d >> 4)
    shl ebx, 2
    shr r13d, 4
    or ebx, r13d
    mov [r10 + rdx], bl
    inc rdx
    jmp .success

.rem_3:
    movzx eax, byte [r8 + rcx]
    movzx ebx, byte [r12 + rax]
    cmp bl, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 1]
    movzx r13d, byte [r12 + rax]
    cmp r13b, 0xFF
    je .fail

    movzx eax, byte [r8 + rcx + 2]
    movzx r14d, byte [r12 + rax]
    cmp r14b, 0xFF
    je .fail

    ; 2 bytes output
    shl ebx, 10
    shl r13d, 4
    shr r14d, 2
    or ebx, r13d
    or ebx, r14d
    mov eax, ebx
    shr eax, 8
    mov [r10 + rdx], al
    mov [r10 + rdx + 1], bl
    add rdx, 2
    jmp .success

.success:
    mov [r11], rdx
    mov byte [r10 + rdx], 0  ; null terminate for strings
    mov eax, 1
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.fail:
    xor eax, eax
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; jwt_verify(const char *token [rdi], size_t token_len [rsi],
;            const char *secret [rdx], size_t secret_len [rcx],
;            struct jwt_info *out_info [r8])
; Returns:
;   0 = success (out_info filled)
;   1 = invalid or expired token (HTTP 401 "invalid or expired token")
;   2 = invalid token payload (HTTP 401 "invalid token payload")
; =============================================================================
jwt_verify:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp

    ; Allocate local buffers:
    ; [rbp - 32]: 32 bytes HMAC output
    ; [rbp - 80]: 48 bytes expected base64url signature
    ; [rbp - 1104]: 1024 bytes decoded payload scratch buffer
    ; [rbp - 1120]: local temp size_t
    sub rsp, 1208

    mov r12, rdi            ; token
    mov r13, rsi            ; token_len
    mov r14, rdx            ; secret
    mov r15, rcx            ; secret_len
    mov [rbp - 1128], r8    ; out_info ptr

    ; 1. Find dot1 and dot2
    xor ecx, ecx            ; index
    mov r8, -1              ; dot1 pos
    mov r9, -1              ; dot2 pos

.find_dots:
    cmp rcx, r13
    jge .check_dots
    movzx eax, byte [r12 + rcx]
    cmp al, '.'
    jne .next_char

    cmp r8, -1
    jne .found_dot2
    mov r8, rcx             ; dot1
    jmp .next_char
.found_dot2:
    cmp r9, -1
    jne .err_invalid_token  ; third dot -> invalid!
    mov r9, rcx             ; dot2
.next_char:
    inc rcx
    jmp .find_dots

.check_dots:
    cmp r8, -1
    je .err_invalid_token
    cmp r9, -1
    je .err_invalid_token
    test r8, r8
    jz .err_invalid_token   ; empty header

    ; Payload is [r8+1 .. r9-1]
    lea rax, [r8 + 1]
    cmp r9, rax
    jle .err_invalid_token  ; empty payload

    ; Save dot1 and dot2 on stack!
    mov [rbp - 1144], r8    ; dot1
    mov [rbp - 1152], r9    ; dot2

    ; Signature is [r9+1 .. r13-1]
    lea rax, [r9 + 1]
    mov rbx, r13
    sub rbx, rax            ; signature len
    cmp rbx, 43             ; HMAC-SHA256 b64url signature is always 43 chars!
    jne .err_invalid_token

    ; 2. Check Header: decode base64url header and check for "HS256"
    ; header slice: [r12] with len r8
    mov rdi, r12
    mov rsi, [rbp - 1144]   ; dot1
    lea rdx, [rbp - 1104]   ; scratch buffer
    lea rcx, [rbp - 1120]   ; out_len
    call b64url_decode
    test eax, eax
    jz .err_invalid_token

    ; Scan decoded header for "HS256"
    mov rax, [rbp - 1120]   ; decoded header len
    lea rdi, [rbp - 1104]   ; decoded header buf
    xor ecx, ecx
    xor edx, edx            ; found flag
.scan_hs256:
    lea rbx, [rcx + 5]
    cmp rbx, rax
    jg .check_hs256_found
    ; Check if "HS256"
    cmp byte [rdi + rcx], 'H'
    jne .hs_next
    cmp byte [rdi + rcx + 1], 'S'
    jne .hs_next
    cmp byte [rdi + rcx + 2], '2'
    jne .hs_next
    cmp byte [rdi + rcx + 3], '5'
    jne .hs_next
    cmp byte [rdi + rcx + 4], '6'
    jne .hs_next
    mov edx, 1              ; found HS256!
    jmp .check_hs256_found
.hs_next:
    inc rcx
    jmp .scan_hs256

.check_hs256_found:
    test edx, edx
    jz .err_invalid_token

    ; 3. Verify Signature
    ; data to sign: token[0 .. dot2] (header + '.' + payload)
    mov rdi, [evp_sha256_md] ; cached EVP_sha256() pointer
    mov rsi, r14            ; secret
    mov edx, r15d           ; secret_len
    mov rcx, r12            ; token
    mov r8, [rbp - 1152]    ; length up to dot2
    lea r9, [rbp - 32]      ; 32-byte md output
    push 0                  ; 7th arg md_len = NULL
    call HMAC
    add rsp, 8

    ; Encode 32-byte md to 43-char b64url
    lea rdi, [rbp - 32]
    lea rsi, [rbp - 80]
    call b64url_encode_32

    ; Compare signature with token[dot2 + 1]
    mov r9, [rbp - 1152]    ; dot2
    lea rdi, [rbp - 80]     ; expected 43-char sig
    lea rsi, [r12 + r9 + 1] ; actual token sig
    xor eax, eax            ; accumulator
    xor ecx, ecx
.cmp_sig:
    cmp ecx, 43
    jge .check_sig_match
    movzx edx, byte [rdi + rcx]
    movzx ebx, byte [rsi + rcx]
    xor edx, ebx
    or eax, edx
    inc ecx
    jmp .cmp_sig

.check_sig_match:
    test eax, eax
    jnz .err_invalid_token  ; signature mismatch!

    ; 4. Decode Payload: token[dot1 + 1 .. dot2]
    mov r8, [rbp - 1144]    ; dot1
    mov r9, [rbp - 1152]    ; dot2
    lea rdi, [r12 + r8 + 1] ; payload start
    mov rsi, r9
    sub rsi, r8
    dec rsi                 ; payload len
    lea rdx, [rbp - 1104]   ; scratch
    lea rcx, [rbp - 1120]   ; out_len
    call b64url_decode
    test eax, eax
    jz .err_invalid_token

    ; 5. Parse Payload JSON: exp, sub, username
    mov r12, [rbp - 1120]   ; payload_len
    lea r13, [rbp - 1104]   ; payload_buf

    ; Check "exp"
    ; Find "exp"
    xor ebx, ebx
.find_exp:
    lea rax, [rbx + 5]
    cmp rax, r12
    jg .err_invalid_token
    cmp byte [r13 + rbx], '"'
    jne .next_exp
    cmp byte [r13 + rbx + 1], 'e'
    jne .next_exp
    cmp byte [r13 + rbx + 2], 'x'
    jne .next_exp
    cmp byte [r13 + rbx + 3], 'p'
    jne .next_exp
    cmp byte [r13 + rbx + 4], '"'
    je .found_exp_key
.next_exp:
    inc rbx
    jmp .find_exp

.found_exp_key:
    add rbx, 5
    ; find ':'
.find_exp_colon:
    cmp rbx, r12
    jge .err_invalid_token
    movzx eax, byte [r13 + rbx]
    cmp al, ':'
    je .past_exp_colon
    inc rbx
    jmp .find_exp_colon
.past_exp_colon:
    inc rbx
    ; skip spaces
.skip_exp_ws:
    cmp rbx, r12
    jge .err_invalid_token
    movzx eax, byte [r13 + rbx]
    cmp al, ' '
    je .next_exp_ws
    jmp .parse_exp_val
.next_exp_ws:
    inc rbx
    jmp .skip_exp_ws

.parse_exp_val:
    ; parse integer until non-digit (comma, space, brace)
    mov rdi, rbx            ; start of number
.scan_exp_digits:
    cmp rbx, r12
    jge .do_parse_exp
    movzx eax, byte [r13 + rbx]
    cmp al, '0'
    jb .do_parse_exp
    cmp al, '9'
    ja .do_parse_exp
    inc rbx
    jmp .scan_exp_digits

.do_parse_exp:
    mov rsi, rbx
    sub rsi, rdi            ; digit length
    jz .err_invalid_token
    lea rdx, [rbp - 1136]   ; out uint64
    lea rdi, [r13 + rdi]
    call str_to_u64
    test eax, eax
    jz .err_invalid_token

    ; Check if exp < current time
    xor edi, edi
    call time
    cmp rax, [rbp - 1136]
    ja .err_invalid_token   ; current_time > exp => expired!

    ; Check "sub"
    ; Must be positive integer string! E.g. "sub":"123"
    xor ebx, ebx
.find_sub:
    lea rax, [rbx + 5]
    cmp rax, r12
    jg .err_invalid_payload
    cmp byte [r13 + rbx], '"'
    jne .next_sub
    cmp byte [r13 + rbx + 1], 's'
    jne .next_sub
    cmp byte [r13 + rbx + 2], 'u'
    jne .next_sub
    cmp byte [r13 + rbx + 3], 'b'
    jne .next_sub
    cmp byte [r13 + rbx + 4], '"'
    je .found_sub_key
.next_sub:
    inc rbx
    jmp .find_sub

.found_sub_key:
    add rbx, 5
.find_sub_colon:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, ':'
    je .past_sub_colon
    inc rbx
    jmp .find_sub_colon
.past_sub_colon:
    inc rbx
.skip_sub_ws:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, ' '
    je .next_sub_ws
    jmp .check_sub_quote
.next_sub_ws:
    inc rbx
    jmp .skip_sub_ws

.check_sub_quote:
    cmp al, '"'
    jne .err_invalid_payload ; sub must be a string!
    inc rbx                  ; start of sub string
    mov rdi, rbx
.scan_sub_end:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, '"'
    je .parse_sub_num
    inc rbx
    jmp .scan_sub_end

.parse_sub_num:
    mov rsi, rbx
    sub rsi, rdi            ; length of sub string
    jz .err_invalid_payload
    lea rdx, [rbp - 1136]   ; user_id
    lea rdi, [r13 + rdi]
    call str_to_u64
    test eax, eax
    jz .err_invalid_payload ; not a valid positive integer string!

    mov r14, [rbp - 1128]   ; out_info
    mov rax, [rbp - 1136]
    mov [r14 + jwt_info.user_id], rax

    ; Check "username"
    ; Must be string! E.g. "username":"golden_ember_1"
    xor ebx, ebx
.find_uname:
    lea rax, [rbx + 10]
    cmp rax, r12
    jg .err_invalid_payload
    ; Check '"username"' (10 chars)
    cmp byte [r13 + rbx], '"'
    jne .next_uname
    mov eax, [r13 + rbx + 1]
    cmp eax, 0x72657375     ; 'u','s','e','r'
    jne .next_uname
    mov eax, [r13 + rbx + 5]
    cmp eax, 0x656d616e     ; 'n','a','m','e'
    jne .next_uname
    cmp byte [r13 + rbx + 9], '"'
    je .found_uname_key
.next_uname:
    inc rbx
    jmp .find_uname

.found_uname_key:
    add rbx, 10
.find_uname_colon:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, ':'
    je .past_uname_colon
    inc rbx
    jmp .find_uname_colon
.past_uname_colon:
    inc rbx
.skip_uname_ws:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, ' '
    je .next_uname_ws
    jmp .check_uname_quote
.next_uname_ws:
    inc rbx
    jmp .skip_uname_ws

.check_uname_quote:
    cmp al, '"'
    jne .err_invalid_payload ; must be string!
    inc rbx                  ; start of username
    mov rdi, rbx
.scan_uname_end:
    cmp rbx, r12
    jge .err_invalid_payload
    movzx eax, byte [r13 + rbx]
    cmp al, '"'
    je .save_uname
    inc rbx
    jmp .scan_uname_end

.save_uname:
    mov rsi, rbx
    sub rsi, rdi            ; username length
    jz .err_invalid_payload
    cmp rsi, 255
    ja .err_invalid_payload

    mov r14, [rbp - 1128]
    mov [r14 + jwt_info.username_len], rsi

    ; copy username bytes
    push rdi
    push rsi
    lea rsi, [r13 + rdi]
    lea rdi, [r14 + jwt_info.username]
    mov rdx, [r14 + jwt_info.username_len]
    cld
.cp_uname:
    test rdx, rdx
    jz .cp_uname_done
    movsb
    dec rdx
    jmp .cp_uname
.cp_uname_done:
    mov byte [rdi], 0       ; null terminate
    pop rsi
    pop rdi

    ; Success!
    xor eax, eax
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.err_invalid_payload:
    mov eax, 2
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.err_invalid_token:
    mov eax, 1
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

section .bss
global evp_sha256_md
evp_sha256_md: resq 1

