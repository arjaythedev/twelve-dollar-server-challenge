; SPDX-License-Identifier: MIT
; Synthetic build-time PGO driver. Not linked into the server; never uses challenge seed.
default rel
extern sqlite3_open_v2,sqlite3_exec,sqlite3_prepare_v3,sqlite3_bind_int64,sqlite3_bind_text
extern sqlite3_step,sqlite3_reset,sqlite3_finalize,sqlite3_close
section .rodata
%include "sql.inc"
%include "train-sql.inc"
training_body: db 'Synthetic training: quotes " and unicode ',0xc3,0xa9,0
training_body_len equ $-training_body-1
section .bss
db: resq 1
stmt: resq 13
section .text
global main
main:
 push rbp
 push rbx
 push r12
 push r13
 push r14
 push r15
 sub rsp,8
 cmp edi,2
 jne .fail
 mov rdi,[rsi+8]
 lea rsi,[db]
 mov edx,6
 xor ecx,ecx
 call sqlite3_open_v2
 test eax,eax
 jnz .fail
 mov rdi,[db]
 lea rsi,[training_schema]
 xor edx,edx
 xor ecx,ecx
 xor r8d,r8d
 call sqlite3_exec
 test eax,eax
 jnz .fail
 xor ebx,ebx
.prepare:
 mov rdi,[db]
 lea rax,[queries]
 mov rsi,[rax+rbx*8]
 mov edx,-1
 mov ecx,1
 lea r8,[stmt]
 lea r8,[r8+rbx*8]
 xor r9d,r9d
 call sqlite3_prepare_v3
 test eax,eax
 jnz .fail
 inc ebx
 cmp ebx,13
 jb .prepare
 mov r12d,2000
.loop:
 mov rdi,[stmt+8]
 call consume
 mov rdi,[stmt+16]
 mov esi,1
 mov edx,r12d
 call sqlite3_bind_int64
 mov rdi,[stmt+16]
 call consume
 test r12d,7
 jnz .next
 mov rdi,[stmt+48]
 call consume
 mov rdi,[stmt+24]
 mov esi,1
 mov edx,1
 call sqlite3_bind_int64
 mov rdi,[stmt+24]
 mov esi,2
 lea rdx,[training_body]
 mov ecx,training_body_len
 xor r8d,r8d
 call sqlite3_bind_text
 mov rdi,[stmt+24]
 call consume
 mov rdi,[stmt+32]
 mov esi,1
 mov edx,1
 call sqlite3_bind_int64
 mov rdi,[stmt+32]
 mov esi,2
 mov edx,r12d
 call sqlite3_bind_int64
 mov rdi,[stmt+32]
 call consume
 mov rdi,[stmt+56]
 call consume
.next:
 dec r12d
 jnz .loop
 xor ebx,ebx
.finalize:
 lea rax,[stmt]
 mov rdi,[rax+rbx*8]
 call sqlite3_finalize
 inc ebx
 cmp ebx,13
 jb .finalize
 mov rdi,[db]
 call sqlite3_close
 xor eax,eax
 jmp .exit
.fail: mov eax,1
.exit:
 add rsp,8
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbx
 pop rbp
 ret
consume:
 push rbx
 mov rbx,rdi
.loop:
 mov rdi,rbx
 call sqlite3_step
 cmp eax,100
 je .loop
 cmp eax,101
 jne .bad
 mov rdi,rbx
 call sqlite3_reset
 test eax,eax
 jnz .bad
 pop rbx
 ret
.bad:
 mov eax,231
 mov edi,1
 syscall
section .note.GNU-stack noalloc noexec nowrite progbits
