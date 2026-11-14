; SPDX-License-Identifier: MIT
default rel
section .text
global abi_call
; abi_call(fn,a,b,c,d) -> 0 when callee-saved registers, DF and caller stack survived.
abi_call:
 push rbp
 push rbx
 push r12
 push r13
 push r14
 push r15
 sub rsp,24
 mov [rsp],rdi
 mov rdi,rsi
 mov rsi,rdx
 mov rdx,rcx
 mov rcx,r8
 mov rax,0x1947928194792819
 mov [rsp+8],rax
 mov rbp,0x1111222233334444
 mov rbx,0x2222333344445555
 mov r12,0x3333444455556666
 mov r13,0x4444555566667777
 mov r14,0x5555666677778888
 mov r15,0x6666777788889999
 call [rsp]
 mov rax,0x1947928194792819
 cmp [rsp+8],rax
 jne .bad
 mov rax,0x1111222233334444
 cmp rbp,rax
 jne .bad
 mov rax,0x2222333344445555
 cmp rbx,rax
 jne .bad
 mov rax,0x3333444455556666
 cmp r12,rax
 jne .bad
 mov rax,0x4444555566667777
 cmp r13,rax
 jne .bad
 mov rax,0x5555666677778888
 cmp r14,rax
 jne .bad
 mov rax,0x6666777788889999
 cmp r15,rax
 jne .bad
 pushfq
 pop rax
 test eax,0x400
 jnz .bad
 xor eax,eax
 jmp .done
.bad:
 cld
 mov eax,-1
.done:
 add rsp,24
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbx
 pop rbp
 ret
section .note.GNU-stack noalloc noexec nowrite progbits
