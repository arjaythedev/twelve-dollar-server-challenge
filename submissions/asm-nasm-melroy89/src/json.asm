; SPDX-License-Identifier: MIT
; Bounded RFC 8259 parser. Decoded strings live in request-local scratch until next parse.
default rel
section .rodata
keys: dq k_alg,k_sub,k_username,k_exp,k_nbf,k_body
k_alg: db 'alg',0
k_sub: db 'sub',0
k_username: db 'username',0
k_exp: db 'exp',0
k_nbf: db 'nbf',0
k_body: db 'body',0
hex: db '0123456789abcdef'
section .bss
align 16
global json_fields
json_fields: resq 18 ; each {type, pointer, byte_length}; string=1, number=2
arena: resb 65536
jdepth: resd 1
section .text
global json_parse, utf8_next, trim_utf8, json_quote, parse_uint, str_len, bytes_equal
str_len:
 xor eax,eax
.loop:
 cmp byte [rdi+rax],0
 je .done
 inc rax
 jmp .loop
.done: ret
; bytes_equal(a,b,n) -> 1 or 0
bytes_equal:
 mov rcx,rdx
 xor eax,eax
 repe cmpsb
 sete al
 ret
; decimal positive integer, <= 2^53-1, exact length; -1 on invalid
parse_uint:
 xor eax,eax
 test rsi,rsi
 jz .bad
 mov r8,9007199254740991
.loop:
 movzx ecx,byte [rdi]
 sub ecx,'0'
 cmp ecx,9
 ja .bad
 imul rax,rax,10
 add rax,rcx
 cmp rax,r8
 ja .bad
 inc rdi
 dec rsi
 jnz .loop
 test rax,rax
 jz .bad
 ret
.bad: mov rax,-1
 ret
; json_parse(input,len) -> type (1..6), -1 malformed. Strings decoded into arena.
json_parse:
 push rbx
 push rbp
 push r12
 push r13
 push r14
 push r15
 sub rsp,8
 mov r12,rdi
 lea r13,[rdi+rsi]
 lea r14,[arena]
 lea r15,[arena+65536]
 lea rdi,[json_fields]
 xor eax,eax
 mov ecx,18
 rep stosq
 mov dword [jdepth],0
 call jvalue
 test eax,eax
 js .exit
 mov ebx,eax
 call jspace
 cmp r12,r13
 jne .bad
 mov eax,ebx
 jmp .exit
.bad: mov eax,-1
.exit:
 add rsp,8
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
jspace:
 cmp r12,r13
 jae .done
 mov al,[r12]
 cmp al,' '
 je .skip
 cmp al,9
 je .skip
 cmp al,10
 je .skip
 cmp al,13
 jne .done
.skip: inc r12
 jmp jspace
.done: ret
jvalue:
 call jspace
 cmp r12,r13
 jae jbad
 mov al,[r12]
 cmp al,'"'
 je jstring
 cmp al,'{'
 je jobject
 cmp al,'['
 je jarray
 cmp al,'t'
 je jtrue
 cmp al,'f'
 je jfalse
 cmp al,'n'
 je jnull
 ; Strict number grammar; caller retains source span.
 mov rdx,r12
 cmp al,'-'
 jne .integer
 inc r12
 cmp r12,r13
 jae jbad
.integer:
 mov al,[r12]
 cmp al,'0'
 jne .nonzero
 inc r12
 jmp .fraction
.nonzero:
 cmp al,'1'
 jb jbad
 cmp al,'9'
 ja jbad
.digits:
 inc r12
 cmp r12,r13
 jae .numdone
 mov al,[r12]
 cmp al,'0'
 jb .fraction
 cmp al,'9'
 jbe .digits
.fraction:
 cmp r12,r13
 jae .numdone
 cmp byte [r12],'.'
 jne .exponent
 inc r12
 call jdigits
 test eax,eax
 js jbad
.exponent:
 cmp r12,r13
 jae .numdone
 mov al,[r12]
 or al,32
 cmp al,'e'
 jne .numdone
 inc r12
 cmp r12,r13
 jae jbad
 mov al,[r12]
 cmp al,'+'
 je .sign
 cmp al,'-'
 jne .edigits
.sign: inc r12
.edigits:
 call jdigits
 test eax,eax
 js jbad
.numdone:
 mov rcx,r12
 sub rcx,rdx
 mov eax,2
 ret
jdigits:
 cmp r12,r13
 jae jbad
 mov al,[r12]
 cmp al,'0'
 jb jbad
 cmp al,'9'
 ja jbad
.loop:
 inc r12
 cmp r12,r13
 jae .done
 mov al,[r12]
 cmp al,'0'
 jb .done
 cmp al,'9'
 jbe .loop
.done: xor eax,eax
 ret
jtrue:
 mov r8d,0x65757274
 mov ecx,4
 jmp jliteral
jfalse:
 mov r8,0x65736c6166
 mov ecx,5
 jmp jliteral
jnull:
 mov r8d,0x6c6c756e
 mov ecx,4
jliteral:
 mov rdx,r12
.loop:
 cmp r12,r13
 jae jbad
 cmp byte [r12],r8b
 jne jbad
 shr r8,8
 inc r12
 dec ecx
 jnz .loop
 mov rcx,r12
 sub rcx,rdx
 mov eax,5
 cmp byte [rdx],'n'
 jne .done
 inc eax
.done: ret
jbad: mov eax,-1
 ret
jobject:
 push rbx
 push rbp
 sub rsp,8
 inc dword [jdepth]
 cmp dword [jdepth],32
 ja .bad
 inc r12
 call jspace
 cmp r12,r13
 jae .bad
 cmp byte [r12],'}'
 je .close
.member:
 cmp r12,r13
 jae .bad
 cmp byte [r12],'"'
 jne .bad
 call jstring
 test eax,eax
 js .bad
 mov ebp,-1
 cmp dword [jdepth],1
 jne .colon
 ; Match top-level keys; no nested member can impersonate top-level claims.
 xor ebx,ebx
.key:
 lea r8,[keys]
 mov r8,[r8+rbx*8]
 xor edi,edi
.cmp:
 mov al,[r8+rdi]
 test al,al
 jz .keyend
 cmp rdi,rcx
 jae .next
 cmp al,[rdx+rdi]
 jne .next
 inc edi
 jmp .cmp
.keyend:
 cmp rdi,rcx
 jne .next
 mov ebp,ebx
 jmp .colon
.next:
 inc ebx
 cmp ebx,6
 jb .key
.colon:
 call jspace
 cmp r12,r13
 jae .bad
 cmp byte [r12],':'
 jne .bad
 inc r12
 call jvalue
 test eax,eax
 js .bad
 test ebp,ebp
 js .separator
 imul ebp,ebp,24
 lea r8,[json_fields]
 mov [r8+rbp],rax
 mov [r8+rbp+8],rdx
 mov [r8+rbp+16],rcx
.separator:
 call jspace
 cmp r12,r13
 jae .bad
 cmp byte [r12],'}'
 je .close
 cmp byte [r12],','
 jne .bad
 inc r12
 call jspace
 jmp .member
.close:
 inc r12
 dec dword [jdepth]
 mov eax,3
 jmp .exit
.bad: mov eax,-1
.exit:
 add rsp,8
 pop rbp
 pop rbx
 ret
jarray:
 push rbx
 inc dword [jdepth]
 cmp dword [jdepth],32
 ja .bad
 inc r12
 call jspace
 cmp r12,r13
 jae .bad
 cmp byte [r12],']'
 je .close
.element:
 call jvalue
 test eax,eax
 js .bad
 call jspace
 cmp r12,r13
 jae .bad
 cmp byte [r12],']'
 je .close
 cmp byte [r12],','
 jne .bad
 inc r12
 jmp .element
.close:
 inc r12
 dec dword [jdepth]
 mov eax,4
 pop rbx
 ret
.bad: mov eax,-1
 pop rbx
 ret
jstring:
 push rbx
 mov rbx,r14
 inc r12
.loop:
 cmp r12,r13
 jae .bad
 cmp r14,r15
 jae .bad
 movzx eax,byte [r12]
 inc r12
 cmp al,'"'
 je .done
 cmp al,32
 jb .bad
 cmp al,92
 je .escape
 mov [r14],al
 inc r14
 jmp .loop
.escape:
 cmp r12,r13
 jae .bad
 mov al,[r12]
 inc r12
 cmp al,'"'
 je .emit
 cmp al,92
 je .emit
 cmp al,'/'
 je .emit
 cmp al,'b'
 je .back
 cmp al,'f'
 je .form
 cmp al,'n'
 je .newline
 cmp al,'r'
 je .cr
 cmp al,'t'
 je .tab
 cmp al,'u'
 jne .bad
 call jhex4
 test eax,eax
 js .bad
 cmp eax,0xd800
 jb .unicode
 cmp eax,0xdbff
 ja .lowcheck
 mov edx,eax
 sub edx,0xd800
 shl edx,10
 lea r8,[r12+6]
 cmp r8,r13
 ja .bad
 cmp word [r12],0x755c
 jne .bad
 add r12,2
 call jhex4
 cmp eax,0xdc00
 jb .bad
 cmp eax,0xdfff
 ja .bad
 sub eax,0xdc00
 add eax,edx
 add eax,0x10000
 jmp .unicode
.lowcheck:
 cmp eax,0xdfff
 jbe .bad
.unicode:
 lea r8,[r14+4]
 cmp r8,r15
 ja .bad
 cmp eax,128
 jb .emit
 cmp eax,2048
 jb .u2
 cmp eax,65536
 jb .u3
 mov ecx,eax
 shr ecx,18
 or cl,0xf0
 mov [r14],cl
 inc r14
 mov ecx,eax
 shr ecx,12
 and cl,63
 or cl,128
 mov [r14],cl
 inc r14
 jmp .u2tail
.u3:
 mov ecx,eax
 shr ecx,12
 or cl,0xe0
 mov [r14],cl
 inc r14
 jmp .u2tail
.u2:
 mov ecx,eax
 shr ecx,6
 or cl,0xc0
 mov [r14],cl
 inc r14
 jmp .utail
.u2tail:
 mov ecx,eax
 shr ecx,6
 and cl,63
 or cl,128
 mov [r14],cl
 inc r14
.utail:
 and al,63
 or al,128
 jmp .emit
.back: mov al,8
 jmp .emit
.form: mov al,12
 jmp .emit
.newline: mov al,10
 jmp .emit
.cr: mov al,13
 jmp .emit
.tab: mov al,9
.emit:
 mov [r14],al
 inc r14
 jmp .loop
.done:
 ; Validate decoded raw UTF-8 too (reject overlong/surrogate sequences).
 mov rdi,rbx
 mov rsi,r14
.validate:
 cmp rdi,rsi
 jae .valid
 call utf8_next
 test eax,eax
 js .bad
 add rdi,rdx
 jmp .validate
.valid:
 mov rdx,rbx
 mov rcx,r14
 sub rcx,rbx
 mov eax,1
 pop rbx
 ret
.bad: mov eax,-1
 pop rbx
 ret
jhex4:
 xor eax,eax
 mov r9d,4
.loop:
 cmp r12,r13
 jae jbad
 movzx ecx,byte [r12]
 inc r12
 cmp cl,'0'
 jb jbad
 cmp cl,'9'
 jbe .digit
 or cl,32
 cmp cl,'a'
 jb jbad
 cmp cl,'f'
 ja jbad
 sub ecx,'a'-10
 jmp .add
.digit: sub ecx,'0'
.add:
 shl eax,4
 or eax,ecx
 dec r9d
 jnz .loop
 ret
; utf8_next(cursor,end) -> codepoint eax, consumed edx; -1 invalid. preserves rdi/rsi.
utf8_next:
 cmp rdi,rsi
 jae .bad
 movzx eax,byte [rdi]
 mov edx,1
 cmp eax,128
 jb .done
 mov r8d,0x80
 mov r9d,2
 cmp eax,0xc2
 jb .bad
 cmp eax,0xdf
 jbe .two
 mov r8d,0x800
 mov r9d,3
 cmp eax,0xef
 jbe .three
 mov r8d,0x10000
 mov r9d,4
 cmp eax,0xf4
 ja .bad
 and eax,7
 jmp .tail
.three: and eax,15
 jmp .tail
.two: and eax,31
.tail:
 lea r10,[rdi+r9]
 cmp r10,rsi
 ja .bad
.loop:
 movzx ecx,byte [rdi+rdx]
 mov r10d,ecx
 and r10d,0xc0
 cmp r10d,0x80
 jne .bad
 and ecx,63
 shl eax,6
 or eax,ecx
 inc edx
 cmp edx,r9d
 jb .loop
 cmp eax,r8d
 jb .bad
 cmp eax,0x10ffff
 ja .bad
 cmp eax,0xd800
 jb .done
 cmp eax,0xdfff
 jbe .bad
.done: ret
.bad: mov eax,-1
 ret
; trim_utf8(data,len) -> rax trimmed pointer, rdx byte length, ecx scalar count; rax=-1 invalid.
trim_utf8:
 push rbx
 push rbp
 push r12
 push r13
 push r14
 mov rbx,rdi
 lea rsi,[rdi+rsi]
 mov rbp,rdi
 mov r12,rdi
 xor r13d,r13d
 xor r14d,r14d
.loop:
 cmp rdi,rsi
 jae .done
 call utf8_next
 test eax,eax
 js .bad
 inc r13d
 cmp eax,32
 je .white
 cmp eax,9
 jb .nonwhite
 cmp eax,13
 jbe .white
 cmp eax,0xa0
 je .white
 cmp eax,0x1680
 je .white
 cmp eax,0x2000
 jb .other
 cmp eax,0x200a
 jbe .white
.other:
 cmp eax,0x2028
 je .white
 cmp eax,0x2029
 je .white
 cmp eax,0x202f
 je .white
 cmp eax,0x205f
 je .white
 cmp eax,0x3000
 je .white
 cmp eax,0xfeff
 je .white
.nonwhite:
 lea r12,[rdi+rdx]
 mov r14d,r13d
 jmp .next
.white:
 cmp r12,rbp
 jne .next
 lea rbp,[rdi+rdx]
 mov r12,rbp
 dec r13d
.next:
 add rdi,rdx
 jmp .loop
.done:
 mov rax,rbp
 mov rdx,r12
 sub rdx,rbp
 mov ecx,r14d
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
.bad:
 mov rax,-1
 pop r14
 pop r13
 pop r12
 pop rbp
 pop rbx
 ret
section .data
global quote_vector_enabled
quote_vector_enabled: dd 0
section .rodata
align 16
quote_marks: times 16 db 34
quote_slashes: times 16 db 92
quote_controls: times 16 db 31
section .text
; json_quote(dst,src,len) -> new dst; caller reserves <=6*len+2 bytes.
json_quote:
 cmp dword [quote_vector_enabled],0
 je json_quote_scalar
 cmp rdx,16
 jb json_quote_scalar
 mov byte [rdi],34
 inc rdi
 lea r8,[hex]
 movdqa xmm5,[quote_marks]
 movdqa xmm6,[quote_slashes]
 movdqa xmm7,[quote_controls]
.block:
 cmp rdx,16
 jb .tail
 ; Full-block loads/stores only while 16 source bytes remain. A tentative
 ; store is inside even the unescaped output bound; escapes overwrite it.
 movdqu xmm0,[rsi]
 movdqa xmm1,xmm0
 pcmpeqb xmm1,xmm5
 movdqa xmm2,xmm0
 pcmpeqb xmm2,xmm6
 movdqa xmm3,xmm0
 pminub xmm3,xmm7
 pcmpeqb xmm3,xmm0
 por xmm1,xmm2
 por xmm1,xmm3
 pmovmskb ecx,xmm1
 movdqu [rdi],xmm0
 test ecx,ecx
 jnz .special
 add rsi,16
 add rdi,16
 sub rdx,16
 jmp .block
.special:
 bsf ecx,ecx
 add rsi,rcx
 add rdi,rcx
 sub rdx,rcx
 movzx eax,byte [rsi]
 inc rsi
 dec rdx
 cmp al,32
 jb .control
.escape:
 mov byte [rdi],92
 mov [rdi+1],al
 add rdi,2
 jmp .block
.control:
 mov dword [rdi],0x3030755c
 mov ecx,eax
 shr ecx,4
 mov cl,[r8+rcx]
 mov [rdi+4],cl
 and eax,15
 mov al,[r8+rax]
 mov [rdi+5],al
 add rdi,6
 jmp .block
.tail:
 test rdx,rdx
 jz .done
 movzx eax,byte [rsi]
 inc rsi
 dec rdx
 cmp al,34
 je .escape
 cmp al,92
 je .escape
 cmp al,32
 jb .control
 mov [rdi],al
 inc rdi
 jmp .tail
.done:
 mov byte [rdi],34
 lea rax,[rdi+1]
 ret
; Original scalar control, retained for paired measurement and short inputs.
global json_quote_scalar
json_quote_scalar:
 mov byte [rdi],'"'
 inc rdi
 lea r8,[hex]
.loop:
 test rdx,rdx
 jz .done
 movzx eax,byte [rsi]
 inc rsi
 dec rdx
 cmp al,'"'
 je .escape
 cmp al,92
 je .escape
 cmp al,32
 jb .control
 mov [rdi],al
 inc rdi
 jmp .loop
.escape:
 mov byte [rdi],92
 mov [rdi+1],al
 add rdi,2
 jmp .loop
.control:
 mov dword [rdi],0x3030755c
 mov ecx,eax
 shr ecx,4
 mov cl,[r8+rcx]
 mov [rdi+4],cl
 and eax,15
 mov al,[r8+rax]
 mov [rdi+5],al
 add rdi,6
 jmp .loop
.done:
 mov byte [rdi],'"'
 lea rax,[rdi+1]
 ret
section .note.GNU-stack noalloc noexec nowrite progbits
section .rodata
ten_double: dq 10.0
one_double: dq 1.0
section .text
global numeric_date
; A fully JSON-validated number span -> xmm0 finite double, eax=0; eax=-1 on overflow.
numeric_date:
 xorpd xmm0,xmm0
 movsd xmm2,[one_double]
 xor r8d,r8d ; negative
 xor r9d,r9d ; fractional digits
 xor r10d,r10d ; exponent
 xor r11d,r11d ; exponent sign
 test rsi,rsi
 jz .bad
 cmp byte [rdi],'-'
 jne .mantissa
 mov r8d,1
 inc rdi
 dec rsi
.mantissa:
 test rsi,rsi
 jz .scale
 movzx eax,byte [rdi]
 inc rdi
 dec rsi
 cmp al,'.'
 je .dot
 or al,32
 cmp al,'e'
 je .exponent
 sub eax,'0'
 cmp eax,9
 ja .bad
 mulsd xmm0,[ten_double]
 cvtsi2sd xmm1,eax
 addsd xmm0,xmm1
 test r9d,r9d
 jz .mantissa
 inc r9d
 jmp .mantissa
.dot:
 mov r9d,1
 jmp .mantissa
.exponent:
 test rsi,rsi
 jz .bad
 cmp byte [rdi],'-'
 jne .plus
 mov r11d,1
 inc rdi
 dec rsi
 jmp .edigit
.plus:
 cmp byte [rdi],'+'
 jne .edigit
 inc rdi
 dec rsi
.edigit:
 test rsi,rsi
 jz .scale
 movzx eax,byte [rdi]
 inc rdi
 dec rsi
 sub eax,'0'
 cmp eax,9
 ja .bad
 imul r10d,r10d,10
 add r10d,eax
 cmp r10d,400
 ja .bad
 jmp .edigit
.scale:
 test r9d,r9d
 jz .esign
 dec r9d
.esign:
 test r11d,r11d
 jz .adjust
 neg r10d
.adjust:
 sub r10d,r9d
 cmp r10d,0
 jge .up
.down:
 divsd xmm0,[ten_double]
 inc r10d
 jnz .down
 jmp .finite
.up:
 test r10d,r10d
 jz .finite
 mulsd xmm0,[ten_double]
 dec r10d
 jmp .up
.finite:
 movq rax,xmm0
 shr rax,52
 and eax,2047
 cmp eax,2047
 je .bad
 test r8d,r8d
 jz .done
 xorpd xmm1,xmm1
 subsd xmm1,xmm0
 movapd xmm0,xmm1
.done:
 xor eax,eax
 ret
.bad:
 mov eax,-1
 ret
section .note.GNU-stack noalloc noexec nowrite progbits
