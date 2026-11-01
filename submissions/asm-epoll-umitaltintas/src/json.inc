; Strict RFC 8259 JSON reading (for request bodies and JWT segments), JSON writing,
; base64url decoding and the JavaScript String.prototype.trim() whitespace set.
;
; Reader convention: rsi = cursor, rdx = end of input. Errors return with CF set.

section .bss
alignb 16
keybuf:     resb SCRATCH_SZ       ; decoded object keys
skipbuf:    resb SCRATCH_SZ       ; decoded strings that are only being skipped
json_depth: resd 1

section .text

; json_ws: skip JSON whitespace. Clobbers al.
json_ws:
  cmp rsi, rdx
  jae .done
  mov al, [rsi]
  cmp al, ' '
  je .skip
  cmp al, 10
  je .skip
  cmp al, 13
  je .skip
  cmp al, 9
  jne .done
.skip:
  inc rsi
  jmp json_ws
.done:
  ret

; hex4: read 4 hex digits at rsi -> eax. Clobbers ecx, r10d.
hex4:
  lea rax, [rsi+4]
  cmp rax, rdx
  ja .bad
  xor eax, eax
  mov ecx, 4
.l:
  movzx r10d, byte [rsi]
  inc rsi
  cmp r10d, '0'
  jb .bad
  cmp r10d, '9'
  jbe .dig
  or r10d, 0x20
  cmp r10d, 'a'
  jb .bad
  cmp r10d, 'f'
  ja .bad
  sub r10d, 'a' - 10
  jmp .acc
.dig:
  sub r10d, '0'
.acc:
  shl eax, 4
  or eax, r10d
  dec ecx
  jnz .l
  clc
  ret
.bad:
  stc
  ret

; json_string: rsi at the opening quote; decodes into rdi (advanced to the end).
; Escapes become UTF-8; surrogate pairs are combined. Clobbers rax, rcx, r8-r10.
json_string:
  inc rsi
.loop:
  cmp rsi, rdx
  jae .err
  movzx eax, byte [rsi]
  cmp al, '"'
  je .done
  cmp al, '\'
  je .esc
  cmp al, 0x20
  jb .err
  mov [rdi], al
  inc rdi
  inc rsi
  jmp .loop
.done:
  inc rsi
  clc
  ret
.err:
  stc
  ret
.esc:
  inc rsi
  cmp rsi, rdx
  jae .err
  movzx eax, byte [rsi]
  inc rsi
  cmp al, 'u'
  je .uni
  cmp al, '"'
  je .put
  cmp al, '\'
  je .put
  cmp al, '/'
  je .put
  mov cl, 8
  cmp al, 'b'
  je .putc
  mov cl, 12
  cmp al, 'f'
  je .putc
  mov cl, 10
  cmp al, 'n'
  je .putc
  mov cl, 13
  cmp al, 'r'
  je .putc
  mov cl, 9
  cmp al, 't'
  je .putc
  jmp .err
.putc:
  mov al, cl
.put:
  mov [rdi], al
  inc rdi
  jmp .loop
.uni:
  call hex4
  jc .err
  cmp eax, 0xD800
  jb .enc
  cmp eax, 0xDBFF
  ja .enc
  ; high surrogate: combine with a following \uDC00-\uDFFF if there is one
  lea rcx, [rsi+6]
  cmp rcx, rdx
  ja .enc
  cmp word [rsi], '\u'
  jne .enc
  mov r9, rsi
  mov r8d, eax
  add rsi, 2
  call hex4
  jc .restore
  cmp eax, 0xDC00
  jb .restore
  cmp eax, 0xDFFF
  ja .restore
  sub r8d, 0xD800
  shl r8d, 10
  sub eax, 0xDC00
  add eax, r8d
  add eax, 0x10000
  jmp .enc
.restore:
  mov rsi, r9
  mov eax, r8d
.enc:
  cmp eax, 0x80
  jb .put
  cmp eax, 0x800
  jb .enc2
  cmp eax, 0x10000
  jb .enc3
  mov ecx, eax
  shr ecx, 18
  or cl, 0xF0
  mov [rdi], cl
  mov ecx, eax
  shr ecx, 12
  and cl, 0x3F
  or cl, 0x80
  mov [rdi+1], cl
  mov ecx, eax
  shr ecx, 6
  and cl, 0x3F
  or cl, 0x80
  mov [rdi+2], cl
  and al, 0x3F
  or al, 0x80
  mov [rdi+3], al
  add rdi, 4
  jmp .loop
.enc3:
  mov ecx, eax
  shr ecx, 12
  or cl, 0xE0
  mov [rdi], cl
  mov ecx, eax
  shr ecx, 6
  and cl, 0x3F
  or cl, 0x80
  mov [rdi+1], cl
  and al, 0x3F
  or al, 0x80
  mov [rdi+2], al
  add rdi, 3
  jmp .loop
.enc2:
  mov ecx, eax
  shr ecx, 6
  or cl, 0xC0
  mov [rdi], cl
  and al, 0x3F
  or al, 0x80
  mov [rdi+1], al
  add rdi, 2
  jmp .loop

; json_lit: rsi at a literal; rdi = expected text, ecx = its length. Clobbers rax.
json_lit:
  lea rax, [rsi+rcx]
  cmp rax, rdx
  ja .bad
.l:
  mov al, [rdi]
  cmp al, [rsi]
  jne .bad
  inc rdi
  inc rsi
  dec ecx
  jnz .l
  clc
  ret
.bad:
  stc
  ret

; json_skip: validate and skip one value at rsi. Clobbers rax, rcx, r8-r10.
json_skip:
  cmp rsi, rdx
  jae .err
  movzx eax, byte [rsi]
  cmp al, '"'
  je .str
  cmp al, '{'
  je .obj
  cmp al, '['
  je .arr
  cmp al, '-'
  je .num
  cmp al, '0'
  jb .lits
  cmp al, '9'
  jbe .num
.lits:
  push rdi
  cmp al, 't'
  je .true
  cmp al, 'f'
  je .false
  cmp al, 'n'
  je .null
  pop rdi
.err:
  stc
  ret
.true:
  lea rdi, [lit_true]
  mov ecx, 4
  jmp .lit
.false:
  lea rdi, [lit_false]
  mov ecx, 5
  jmp .lit
.null:
  lea rdi, [lit_null]
  mov ecx, 4
.lit:
  call json_lit
  pop rdi
  ret
.str:
  push rdi
  lea rdi, [skipbuf]
  call json_string
  pop rdi
  ret
.num:
  cmp al, '-'
  jne .int
  inc rsi
  cmp rsi, rdx
  jae .err
.int:
  movzx eax, byte [rsi]
  cmp al, '0'
  je .zero
  cmp al, '1'
  jb .err
  cmp al, '9'
  ja .err
  call .digits
  jmp .frac
.zero:
  inc rsi
.frac:
  cmp rsi, rdx
  jae .numok
  cmp byte [rsi], '.'
  jne .exp
  inc rsi
  call .digits1
  jc .err
.exp:
  cmp rsi, rdx
  jae .numok
  mov al, [rsi]
  or al, 0x20
  cmp al, 'e'
  jne .numok
  inc rsi
  cmp rsi, rdx
  jae .err
  mov al, [rsi]
  cmp al, '+'
  je .sign
  cmp al, '-'
  jne .edig
.sign:
  inc rsi
.edig:
  call .digits1
  jc .err
.numok:
  clc
  ret
.digits1:                     ; one or more digits
  cmp rsi, rdx
  jae .d_bad
  mov al, [rsi]
  sub al, '0'
  cmp al, 9
  ja .d_bad
.digits:                      ; zero or more digits
  cmp rsi, rdx
  jae .d_ok
  mov al, [rsi]
  sub al, '0'
  cmp al, 9
  ja .d_ok
  inc rsi
  jmp .digits
.d_ok:
  clc
  ret
.d_bad:
  stc
  ret
.obj:
  inc dword [json_depth]
  cmp dword [json_depth], 256
  ja .err
  inc rsi
  call json_ws
  cmp rsi, rdx
  jae .err
  cmp byte [rsi], '}'
  je .close
.member:
  cmp rsi, rdx
  jae .err
  cmp byte [rsi], '"'
  jne .err
  push rdi
  lea rdi, [skipbuf]
  call json_string
  pop rdi
  jc .err
  call json_ws
  cmp rsi, rdx
  jae .err
  cmp byte [rsi], ':'
  jne .err
  inc rsi
  call json_ws
  call json_skip
  jc .err
  call json_ws
  cmp rsi, rdx
  jae .err
  mov al, [rsi]
  inc rsi
  cmp al, '}'
  je .closed
  cmp al, ','
  jne .err
  call json_ws
  jmp .member
.arr:
  inc dword [json_depth]
  cmp dword [json_depth], 256
  ja .err
  inc rsi
  call json_ws
  cmp rsi, rdx
  jae .err
  cmp byte [rsi], ']'
  je .close
.elem:
  call json_skip
  jc .err
  call json_ws
  cmp rsi, rdx
  jae .err
  mov al, [rsi]
  inc rsi
  cmp al, ']'
  je .closed
  cmp al, ','
  jne .err
  call json_ws
  jmp .elem
.close:
  inc rsi
.closed:
  dec dword [json_depth]
  clc
  ret

; obj_scan: validate a whole JSON document (rsi..rdx) and, if it is an object,
; record where the values of the keys in the table at rdi start (last one wins).
; Table: dd count, pad; then per key: dq key_ptr, key_len, value_ptr (output, 0 if absent).
; Returns CF=1 if the document is not valid JSON, else eax=1 for an object, 0 otherwise.
obj_scan:
  push rbx
  push r12
  push r13
  push r14
  mov r12, rdi
  mov dword [json_depth], 0
  mov ecx, [r12]
  lea rax, [r12+8]
.clr:
  test ecx, ecx
  jz .cleared
  mov qword [rax+16], 0
  add rax, 24
  dec ecx
  jmp .clr
.cleared:
  call json_ws
  cmp rsi, rdx
  jae .bad
  cmp byte [rsi], '{'
  jne .notobj
  inc rsi
  call json_ws
  cmp rsi, rdx
  jae .bad
  cmp byte [rsi], '}'
  jne .member
  inc rsi
  jmp .end
.member:
  cmp rsi, rdx
  jae .bad
  cmp byte [rsi], '"'
  jne .bad
  lea rdi, [keybuf]
  call json_string
  jc .bad
  mov rbx, rdi
  lea rax, [keybuf]
  sub rbx, rax                ; key length
  call json_ws
  cmp rsi, rdx
  jae .bad
  cmp byte [rsi], ':'
  jne .bad
  inc rsi
  call json_ws
  cmp rsi, rdx
  jae .bad
  mov r14d, [r12]
  lea r13, [r12+8]
.km:
  test r14d, r14d
  jz .kdone
  cmp rbx, [r13+8]
  jne .knext
  push rsi
  mov rsi, [r13]
  lea rdi, [keybuf]
  mov rcx, rbx
  repe cmpsb
  pop rsi
  jne .knext
  mov [r13+16], rsi
.knext:
  add r13, 24
  dec r14d
  jmp .km
.kdone:
  call json_skip
  jc .bad
  call json_ws
  cmp rsi, rdx
  jae .bad
  mov al, [rsi]
  inc rsi
  cmp al, '}'
  je .end
  cmp al, ','
  jne .bad
  call json_ws
  jmp .member
.end:
  call json_ws
  cmp rsi, rdx
  jne .bad
  mov eax, 1
  clc
  jmp .ret
.notobj:
  call json_skip
  jc .bad
  call json_ws
  cmp rsi, rdx
  jne .bad
  xor eax, eax
  clc
  jmp .ret
.bad:
  stc
.ret:
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; b64d: base64url (no padding) rsi/rdx(len) -> rdi. Returns rax = bytes or -1.
; Clobbers rcx, r8-r10.
b64d:
  lea r8, [b64tab]
  mov r9, rdi
  mov eax, edx
  and eax, 3
  cmp eax, 1
  je .bad
  xor ecx, ecx
  xor r10d, r10d
  test rdx, rdx
  jz .done
.l:
  movzx eax, byte [rsi]
  movzx eax, byte [r8+rax]
  cmp al, 0xFF
  je .bad
  shl r10d, 6
  or r10d, eax
  add ecx, 6
  cmp ecx, 8
  jb .n
  sub ecx, 8
  mov eax, r10d
  shr eax, cl
  mov [rdi], al
  inc rdi
.n:
  inc rsi
  dec rdx
  jnz .l
.done:
  mov rax, rdi
  sub rax, r9
  ret
.bad:
  mov rax, -1
  ret

; b64_init: build the decode table
b64_init:
  lea rdi, [b64tab]
  mov al, 0xFF
  mov ecx, 256
  rep stosb
  lea rdi, [b64tab]
  xor eax, eax
  mov ecx, 'A'
.u:
  mov [rdi+rcx], al
  inc eax
  inc ecx
  cmp ecx, 'Z'
  jbe .u
  mov ecx, 'a'
.lo:
  mov [rdi+rcx], al
  inc eax
  inc ecx
  cmp ecx, 'z'
  jbe .lo
  mov ecx, '0'
.d:
  mov [rdi+rcx], al
  inc eax
  inc ecx
  cmp ecx, '9'
  jbe .d
  mov byte [rdi+'-'], 62
  mov byte [rdi+'_'], 63
  ret

; w_u64: append rax as decimal at rdi. Clobbers rax, rcx, rdx, rsi, r8, r9 (uses the red zone).
; Divides by 10 with a multiply (x * ceil(2^67 / 10) >> 67), not div.
w_u64:
  lea rsi, [rsp-1]
  mov r8, rsi
  mov r9, rax
  mov rcx, 0xCCCCCCCCCCCCCCCD
.l:
  mov rax, r9
  mul rcx
  shr rdx, 3                  ; rdx = r9 / 10
  lea rax, [rdx + rdx*4]
  add rax, rax                ; rax = (r9 / 10) * 10
  sub r9, rax                 ; r9 = digit
  add r9b, '0'
  mov [rsi], r9b
  dec rsi
  mov r9, rdx
  test rdx, rdx
  jnz .l
  mov rcx, r8
  sub rcx, rsi
  inc rsi
  rep movsb
  ret

; w_jstr: append rsi/rdx(len) as a quoted JSON string at rdi.
; Escapes '"', '\' and control characters like JSON.stringify. 16 bytes at a time while none of
; them needs escaping (the output buffer must have 16 bytes of room past the result).
; Clobbers rax, rcx, rdx, rsi, r8, xmm0-xmm5.
w_jstr:
  mov byte [rdi], '"'
  inc rdi
  test rdx, rdx
  jz .end
  lea r8, [rsi+rdx]
  movdqa xmm1, [jq_quote]
  movdqa xmm2, [jq_bslash]
  movdqa xmm3, [jq_ctl]
.l:
  lea rax, [rsi+16]
  cmp rax, r8
  ja .tail
  movdqu xmm0, [rsi]
  movdqa xmm4, xmm0
  pcmpeqb xmm4, xmm1          ; '"'
  movdqa xmm5, xmm0
  pcmpeqb xmm5, xmm2          ; '\'
  por xmm4, xmm5
  movdqa xmm5, xmm0
  pminub xmm5, xmm3
  pcmpeqb xmm5, xmm0          ; byte <= 0x1F
  por xmm4, xmm5
  pmovmskb eax, xmm4
  movdqu [rdi], xmm0
  test eax, eax
  jnz .some
  add rsi, 16
  add rdi, 16
  jmp .l
.some:                        ; the clean bytes before the first special one are copied
  bsf ecx, eax
  add rsi, rcx
  add rdi, rcx
  movzx eax, byte [rsi]
  jmp .chk
.tail:
  cmp rsi, r8
  jae .end
  movzx eax, byte [rsi]
.chk:
  cmp al, '"'
  je .q
  cmp al, '\'
  je .q
  cmp al, 0x20
  jb .ctl
  mov [rdi], al
  inc rdi
.n:
  inc rsi
  jmp .l
.end:
  mov byte [rdi], '"'
  inc rdi
  ret
.q:
  mov byte [rdi], '\'
  mov [rdi+1], al
  add rdi, 2
  jmp .n
.ctl:
  lea rcx, [ctl_esc]
  movzx ecx, byte [rcx+rax]
  test ecx, ecx
  jz .u
  mov byte [rdi], '\'
  mov [rdi+1], cl
  add rdi, 2
  jmp .n
.u:
  mov dword [rdi], '\u00'
  lea rcx, [hexdig]
  mov ah, al
  shr al, 4
  and ah, 15
  movzx edx, al
  mov dl, [rcx+rdx]
  mov [rdi+4], dl
  movzx edx, ah
  mov dl, [rcx+rdx]
  mov [rdi+5], dl
  add rdi, 6
  jmp .n

[section .rodata]
align 16
jq_quote:  times 16 db '"'
jq_bslash: times 16 db '\'
jq_ctl:    times 16 db 0x1F
__?SECT?__

; ws_len: length of a JS whitespace character at rsi (rdi = end), 0 if none.
; ws_len_back: same for the character that ends at rdi (rsi = start). Clobber eax, ecx.
ws_len:
  mov rcx, rdi
  sub rcx, rsi
  jz .no
  movzx eax, byte [rsi]
  call is_ws1
  jz .one
  cmp rcx, 2
  jb .no
  cmp word [rsi], 0xA0C2
  je .two
  cmp rcx, 3
  jb .no
  movzx eax, word [rsi]
  movzx ecx, byte [rsi+2]
  shl ecx, 16
  or eax, ecx
  call is_ws3
  jz .three
.no:
  xor eax, eax
  ret
.one:
  mov eax, 1
  ret
.two:
  mov eax, 2
  ret
.three:
  mov eax, 3
  ret

ws_len_back:
  mov rcx, rdi
  sub rcx, rsi
  jz ws_len.no
  movzx eax, byte [rdi-1]
  call is_ws1
  jz ws_len.one
  cmp rcx, 2
  jb ws_len.no
  cmp word [rdi-2], 0xA0C2
  je ws_len.two
  cmp rcx, 3
  jb ws_len.no
  movzx eax, word [rdi-3]
  movzx ecx, byte [rdi-1]
  shl ecx, 16
  or eax, ecx
  call is_ws3
  jz ws_len.three
  jmp ws_len.no

; is_ws1: ZF=1 if al is ASCII whitespace (\t \n \v \f \r space)
is_ws1:
  cmp al, ' '
  je .y
  cmp al, 9
  jb .n
  cmp al, 13
  ja .n
  cmp al, al
.y:
  ret
.n:                          ; reached through jb/ja, so ZF is already 0
  ret

; is_ws3: ZF=1 if eax (3 UTF-8 bytes, little endian) is one of the 3-byte JS whitespace chars
is_ws3:
  cmp eax, 0x809AE1           ; U+1680
  je .y
  cmp eax, 0x9F81E2           ; U+205F
  je .y
  cmp eax, 0x8080E3           ; U+3000
  je .y
  cmp eax, 0xBFBBEF           ; U+FEFF
  je .y
  mov ecx, eax
  and ecx, 0xFFFF
  cmp ecx, 0x80E2
  jne .n
  mov ecx, eax
  shr ecx, 16
  cmp ecx, 0xA8               ; U+2028
  je .y
  cmp ecx, 0xA9               ; U+2029
  je .y
  cmp ecx, 0xAF               ; U+202F
  je .y
  cmp ecx, 0x80
  jb .n
  cmp ecx, 0x8A               ; U+2000..U+200A
  ja .n
  cmp ecx, ecx
.y:
  ret
.n:                          ; reached through jne/jb/ja, so ZF is already 0
  ret

; trim: rsi = start, rdi = end -> trimmed rsi/rdi
trim:
.front:
  call ws_len
  test eax, eax
  jz .back
  add rsi, rax
  jmp .front
.back:
  call ws_len_back
  test eax, eax
  jz .done
  sub rdi, rax
  jmp .back
.done:
  ret

section .rodata
lit_true:  db 'true'
lit_false: db 'false'
lit_null:  db 'null'
hexdig:    db '0123456789abcdef'
ctl_esc:   db 0,0,0,0,0,0,0,0,'b','t','n',0,'f','r',0,0
           times 16 db 0

section .bss
b64tab: resb 256
