; A validating JSON reader (RFC 8259, UTF-8 checked) that reports chosen top-level members, and
; a string unescaper. Used for request bodies and for JWT headers and payloads.
%include "defs.inc"

global json_obj, json_unescape

section .bss
json_sp:   resq 1
keybuf:    resb 72

section .text

; Errors anywhere in the scan jump here: the stack is unwound to json_obj's frame.
json_fail:
  mov rsp, [json_sp]
  mov eax, -1
  jmp json_obj.ret

; ---- json_obj(rdi = text, rsi = len, rdx = keys {ptr,len}[n], rcx = slots[n], r8 = n) ----------
; eax = -1 malformed, 0 valid JSON that isn't an object, 1 an object. For each key found at the
; top level, its slot gets {type, start, len, escaped}; the last duplicate wins.
json_obj:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 40                    ; [0] key start, [8] key len, [16] key esc
  mov [json_sp], rsp
  mov r12, rdi
  lea r13, [rdi + rsi]
  mov r14, rdx
  mov r15, rcx
  mov rbx, r8
  ; clear the slots
  mov rcx, rbx
  shl rcx, 5
  mov rdi, r15
  xor eax, eax
  rep stosb
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], '{'
  je .object
  xor ecx, ecx
  call jval
  call jws
  cmp r12, r13
  jne json_fail
  xor eax, eax
  jmp .ret
.object:
  inc r12
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], '}'
  je .close
.member:
  cmp r12, r13
  jae json_fail
  cmp byte [r12], '"'
  jne json_fail
  call jstr
  mov [rsp], rax
  mov rcx, r12
  sub rcx, rax
  dec rcx
  mov [rsp + 8], rcx
  mov [rsp + 16], rdx
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], ':'
  jne json_fail
  inc r12
  call jws
  mov ecx, 1
  call jval                      ; eax type, r8 start, r9 len, r10 esc
  mov [rsp + 24], r8
  mov [rsp + 32], r9
  mov ebp, eax
  ; the key, decoded if it has escapes (a key over 64 bytes can't be one we want)
  mov rsi, [rsp]
  mov rdx, [rsp + 8]
  cmp qword [rsp + 16], 0
  je .match
  cmp rdx, 64
  ja .next
  lea rdi, [keybuf]
  push r10
  push r10
  call json_unescape
  pop r10
  pop r10
  lea rsi, [keybuf]
  mov rdx, rax
  sub rdx, rsi
.match:
  xor ecx, ecx
.mk:
  cmp rcx, rbx
  jae .next
  mov rdi, rcx
  shl rdi, 4
  cmp rdx, [r14 + rdi + 8]
  jne .mk_no
  mov rdi, [r14 + rdi]
  xor eax, eax
.cmp:
  cmp rax, rdx
  jae .found
  movzx r11d, byte [rsi + rax]
  cmp r11b, [rdi + rax]
  jne .mk_no
  inc rax
  jmp .cmp
.found:
  mov rdi, rcx
  shl rdi, 5
  mov [r15 + rdi + J_TYPE], rbp
  mov rax, [rsp + 24]
  mov [r15 + rdi + J_START], rax
  mov rax, [rsp + 32]
  mov [r15 + rdi + J_LEN], rax
  mov [r15 + rdi + J_ESC], r10
.mk_no:
  inc rcx
  jmp .mk
.next:
  call jws
  cmp r12, r13
  jae json_fail
  mov al, [r12]
  inc r12
  cmp al, ','
  jne .endobj
  call jws
  jmp .member
.endobj:
  cmp al, '}'
  jne json_fail
  dec r12
.close:
  inc r12
  call jws
  cmp r12, r13
  jne json_fail
  mov eax, 1
.ret:
  add rsp, 40
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; ---- internal scanner: r12 = cursor, r13 = end -------------------------------------------------

; skips whitespace
jws:
  cmp r12, r13
  jae .d
  movzx eax, byte [r12]
  cmp al, ' '
  je .s
  cmp al, 10
  je .s
  cmp al, 13
  je .s
  cmp al, 9
  jne .d
.s:
  inc r12
  jmp jws
.d:
  ret

; jval(ecx = depth): one value at r12. eax = type (1 string, 2 number, 3 other),
; r8 = start, r9 = length (inside the quotes for strings), r10 = escapes (strings).
jval:
  cmp r12, r13
  jae json_fail
  movzx eax, byte [r12]
  cmp al, '"'
  je .str
  cmp al, '{'
  je .obj
  cmp al, '['
  je .arr
  cmp al, 't'
  je .true
  cmp al, 'f'
  je .false
  cmp al, 'n'
  je .null
  cmp al, '-'
  je .num
  sub al, '0'
  cmp al, 9
  jbe .num
  jmp json_fail
.str:
  call jstr
  mov r8, rax
  mov r9, r12
  sub r9, rax
  dec r9
  mov r10, rdx
  mov eax, 1
  ret
.num:
  mov r8, r12
  cmp byte [r12], '-'
  jne .int
  inc r12
.int:
  cmp r12, r13
  jae json_fail
  movzx eax, byte [r12]
  cmp al, '0'
  jne .nz
  inc r12
  jmp .frac
.nz:
  call jdigits
.frac:
  cmp r12, r13
  jae .numdone
  cmp byte [r12], '.'
  jne .exp
  inc r12
  call jdigits
  cmp r12, r13
  jae .numdone
.exp:
  movzx eax, byte [r12]
  or al, 0x20
  cmp al, 'e'
  jne .numdone
  inc r12
  cmp r12, r13
  jae json_fail
  movzx eax, byte [r12]
  cmp al, '+'
  je .sign
  cmp al, '-'
  jne .ed
.sign:
  inc r12
.ed:
  call jdigits
.numdone:
  mov r9, r12
  sub r9, r8
  xor r10d, r10d
  mov eax, 2
  ret
.true:
  mov r8, r12
  lea rax, [r12 + 4]
  cmp rax, r13
  ja json_fail
  cmp dword [r12], 'true'
  jne json_fail
  mov r12, rax
  jmp .other
.null:
  mov r8, r12
  lea rax, [r12 + 4]
  cmp rax, r13
  ja json_fail
  cmp dword [r12], 'null'
  jne json_fail
  mov r12, rax
  jmp .other
.false:
  mov r8, r12
  lea rax, [r12 + 5]
  cmp rax, r13
  ja json_fail
  cmp dword [r12], 'fals'
  jne json_fail
  cmp byte [r12 + 4], 'e'
  jne json_fail
  mov r12, rax
.other:
  mov r9, r12
  sub r9, r8
  xor r10d, r10d
  mov eax, 3
  ret
.arr:
  inc ecx
  cmp ecx, 512
  ja json_fail
  push r8
  mov r8, r12
  push r8                        ; start
  inc r12
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], ']'
  je .arr_end
.arr_item:
  push rcx
  call jval
  pop rcx
  call jws
  cmp r12, r13
  jae json_fail
  mov al, [r12]
  inc r12
  cmp al, ','
  jne .arr_close
  call jws
  jmp .arr_item
.arr_close:
  cmp al, ']'
  jne json_fail
  dec r12
.arr_end:
  inc r12
  pop r8
  add rsp, 8
  dec ecx
  jmp .other
.obj:
  inc ecx
  cmp ecx, 512
  ja json_fail
  push r8
  mov r8, r12
  push r8
  inc r12
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], '}'
  je .obj_end
.obj_mem:
  cmp r12, r13
  jae json_fail
  cmp byte [r12], '"'
  jne json_fail
  call jstr
  call jws
  cmp r12, r13
  jae json_fail
  cmp byte [r12], ':'
  jne json_fail
  inc r12
  call jws
  push rcx
  call jval
  pop rcx
  call jws
  cmp r12, r13
  jae json_fail
  mov al, [r12]
  inc r12
  cmp al, ','
  jne .obj_close
  call jws
  jmp .obj_mem
.obj_close:
  cmp al, '}'
  jne json_fail
  dec r12
.obj_end:
  inc r12
  pop r8
  add rsp, 8
  dec ecx
  jmp .other

; one or more digits
jdigits:
  cmp r12, r13
  jae json_fail
  movzx eax, byte [r12]
  sub al, '0'
  cmp al, 9
  ja json_fail
.more:
  inc r12
  cmp r12, r13
  jae .d
  movzx eax, byte [r12]
  sub al, '0'
  cmp al, 9
  jbe .more
.d:
  ret

; jstr: r12 at the opening quote. Returns rax = first byte inside, rdx = 1 if it has escapes;
; r12 after the closing quote. Rejects control characters, bad escapes and invalid UTF-8.
jstr:
  inc r12
  mov rax, r12
  xor edx, edx
  mov r11d, 0x1f1f1f1f
  movd xmm3, r11d
  pshufd xmm3, xmm3, 0           ; 0x1f x16
  mov r11d, 0x22222222
  movd xmm4, r11d
  pshufd xmm4, xmm4, 0           ; '"' x16
  mov r11d, 0x5c5c5c5c
  movd xmm5, r11d
  pshufd xmm5, xmm5, 0           ; '\' x16
.fast:
  lea r11, [r12 + 16]
  cmp r11, r13
  ja .slow
  movdqu xmm0, [r12]
  movdqa xmm1, xmm0
  pcmpgtb xmm1, xmm3             ; 0x20..0x7f (signed > 0x1f): plain
  movdqa xmm2, xmm0
  pcmpeqb xmm2, xmm4
  pandn xmm2, xmm1               ; plain and not '"'
  movdqa xmm1, xmm0
  pcmpeqb xmm1, xmm5
  pandn xmm1, xmm2               ; ... and not '\'
  pmovmskb r11d, xmm1
  xor r11d, 0xffff
  jnz .stop
  add r12, 16
  jmp .fast
.stop:
  bsf r11d, r11d
  add r12, r11
.slow:
  cmp r12, r13
  jae json_fail
  movzx r11d, byte [r12]
  cmp r11b, '"'
  je .end
  cmp r11b, '\'
  je .esc
  cmp r11b, 0x20
  jb json_fail
  cmp r11b, 0x80
  jae .utf8
  inc r12
  jmp .fast
.end:
  inc r12
  ret
.esc:
  mov edx, 1
  lea r11, [r12 + 2]
  cmp r11, r13
  ja json_fail
  movzx r11d, byte [r12 + 1]
  cmp r11b, 'u'
  je .uesc
  cmp r11b, '"'
  je .e2
  cmp r11b, '\'
  je .e2
  cmp r11b, '/'
  je .e2
  cmp r11b, 'b'
  je .e2
  cmp r11b, 'f'
  je .e2
  cmp r11b, 'n'
  je .e2
  cmp r11b, 'r'
  je .e2
  cmp r11b, 't'
  jne json_fail
.e2:
  add r12, 2
  jmp .fast
.uesc:
  lea r11, [r12 + 6]
  cmp r11, r13
  ja json_fail
  push rcx
  mov ecx, 2
.hex:
  movzx r11d, byte [r12 + rcx]
  sub r11b, '0'
  cmp r11b, 9
  jbe .hok
  movzx r11d, byte [r12 + rcx]
  or r11b, 0x20
  sub r11b, 'a'
  cmp r11b, 5
  ja .hbad
.hok:
  inc ecx
  cmp ecx, 6
  jb .hex
  pop rcx
  add r12, 6
  jmp .fast
.hbad:
  pop rcx
  jmp json_fail
.utf8:
  ; r11 = lead byte >= 0x80
  cmp r11b, 0xc2
  jb json_fail
  cmp r11b, 0xe0
  jb .u2
  cmp r11b, 0xf0
  jb .u3
  cmp r11b, 0xf5
  jae json_fail
  ; 4 bytes
  lea r10, [r12 + 4]
  cmp r10, r13
  ja json_fail
  movzx r10d, byte [r12 + 1]
  cmp r11b, 0xf0
  jne .u4a
  sub r10b, 0x90
  cmp r10b, 0x2f
  ja json_fail
  jmp .u4b
.u4a:
  cmp r11b, 0xf4
  jne .u4c
  sub r10b, 0x80
  cmp r10b, 0x0f
  ja json_fail
  jmp .u4b
.u4c:
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
.u4b:
  movzx r10d, byte [r12 + 2]
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
  movzx r10d, byte [r12 + 3]
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
  add r12, 4
  jmp .fast
.u3:
  lea r10, [r12 + 3]
  cmp r10, r13
  ja json_fail
  movzx r10d, byte [r12 + 1]
  cmp r11b, 0xe0
  jne .u3a
  sub r10b, 0xa0
  cmp r10b, 0x1f
  ja json_fail
  jmp .u3b
.u3a:
  cmp r11b, 0xed
  jne .u3c
  sub r10b, 0x80
  cmp r10b, 0x1f
  ja json_fail
  jmp .u3b
.u3c:
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
.u3b:
  movzx r10d, byte [r12 + 2]
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
  add r12, 3
  jmp .fast
.u2:
  lea r10, [r12 + 2]
  cmp r10, r13
  ja json_fail
  movzx r10d, byte [r12 + 1]
  sub r10b, 0x80
  cmp r10b, 0x3f
  ja json_fail
  add r12, 2
  jmp .fast

; ---- json_unescape(rdi = dst, rsi = src, rdx = len) -> rax = end of output ---------------------
; src is the inside of a string json_obj accepted. A lone surrogate becomes U+FFFD.
json_unescape:
  lea r8, [rsi + rdx]
.loop:
  cmp rsi, r8
  jae .done
  movzx eax, byte [rsi]
  cmp al, '\'
  je .esc
  mov [rdi], al
  inc rsi
  inc rdi
  jmp .loop
.esc:
  movzx eax, byte [rsi + 1]
  add rsi, 2
  cmp al, 'u'
  je .u
  mov ecx, 8
  cmp al, 'b'
  je .put
  mov ecx, 12
  cmp al, 'f'
  je .put
  mov ecx, 10
  cmp al, 'n'
  je .put
  mov ecx, 13
  cmp al, 'r'
  je .put
  mov ecx, 9
  cmp al, 't'
  je .put
  mov ecx, eax                   ; " \ /
.put:
  mov [rdi], cl
  inc rdi
  jmp .loop
.u:
  call .hex4                     ; ecx = code unit, rsi += 4
  mov eax, ecx
  and eax, 0xfc00
  cmp eax, 0xdc00
  je .lone
  cmp eax, 0xd800
  jne .enc
  ; a high surrogate: needs \uDC00-\uDFFF next
  lea rax, [rsi + 6]
  cmp rax, r8
  ja .lone
  cmp byte [rsi], '\'
  jne .lone
  cmp byte [rsi + 1], 'u'
  jne .lone
  mov r9d, ecx
  add rsi, 2
  call .hex4
  mov eax, ecx
  and eax, 0xfc00
  cmp eax, 0xdc00
  je .pair
  sub rsi, 6                     ; not a low surrogate: re-read it as its own escape
  mov ecx, 0xfffd
  jmp .enc
.pair:
  mov eax, r9d
  sub eax, 0xd800
  shl eax, 10
  sub ecx, 0xdc00
  add ecx, eax
  add ecx, 0x10000
  jmp .enc
.lone:
  mov ecx, 0xfffd
.enc:
  cmp ecx, 0x80
  jae .e2
  mov [rdi], cl
  inc rdi
  jmp .loop
.e2:
  cmp ecx, 0x800
  jae .e3
  mov eax, ecx
  shr eax, 6
  or al, 0xc0
  mov [rdi], al
  mov eax, ecx
  and al, 0x3f
  or al, 0x80
  mov [rdi + 1], al
  add rdi, 2
  jmp .loop
.e3:
  cmp ecx, 0x10000
  jae .e4
  mov eax, ecx
  shr eax, 12
  or al, 0xe0
  mov [rdi], al
  mov eax, ecx
  shr eax, 6
  and al, 0x3f
  or al, 0x80
  mov [rdi + 1], al
  mov eax, ecx
  and al, 0x3f
  or al, 0x80
  mov [rdi + 2], al
  add rdi, 3
  jmp .loop
.e4:
  mov eax, ecx
  shr eax, 18
  or al, 0xf0
  mov [rdi], al
  mov eax, ecx
  shr eax, 12
  and al, 0x3f
  or al, 0x80
  mov [rdi + 1], al
  mov eax, ecx
  shr eax, 6
  and al, 0x3f
  or al, 0x80
  mov [rdi + 2], al
  mov eax, ecx
  and al, 0x3f
  or al, 0x80
  mov [rdi + 3], al
  add rdi, 4
  jmp .loop
.done:
  mov rax, rdi
  ret
; four hex digits at rsi -> ecx; rsi += 4
.hex4:
  xor ecx, ecx
  mov r10d, 4
.h:
  movzx eax, byte [rsi]
  inc rsi
  cmp al, '9'
  jbe .dig
  or al, 0x20
  sub al, 'a' - 10
  jmp .acc
.dig:
  sub al, '0'
.acc:
  shl ecx, 4
  or ecx, eax
  dec r10d
  jnz .h
  ret
