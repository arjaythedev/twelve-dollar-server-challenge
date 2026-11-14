; SPDX-License-Identifier: MIT
; Test-only SQLite boundary fault injector, never linked into the submitted server.
default rel
extern __real_sqlite3_step,sqlite3_sql,sqlite3_db_handle,sqlite3_exec
section .data
%ifdef ROLLBACK_TEST
remaining: dd 3
%else
remaining: dd 1
%endif
section .rodata
rollback_sql: db 'ROLLBACK',0
section .text
global __wrap_sqlite3_step
__wrap_sqlite3_step:
 push rbx
 mov rbx,rdi
 call sqlite3_sql
 test rax,rax
 jz .real
%ifdef ROLLBACK_TEST
 cmp dword [rax],0x45534e49 ; INSE
 jne .real
%else
 cmp word [rax],0x4f43 ; CO
 jne .real
 cmp dword [rax+2],0x54494d4d ; MMIT
 jne .real
 cmp byte [rax+6],0
 jne .real
%endif
 cmp dword [remaining],0
 je .real
 dec dword [remaining]
 jnz .real
%ifdef ROLLBACK_TEST
 ; Mimic a storage failure that aborts the complete SQLite transaction.
 mov rdi,rbx
 call sqlite3_db_handle
 mov rdi,rax
 lea rsi,[rollback_sql]
 xor edx,edx
 xor ecx,ecx
 xor r8d,r8d
 call sqlite3_exec
 mov eax,13 ; SQLITE_FULL
%else
 mov eax,10 ; SQLITE_IOERR before COMMIT actually executes
%endif
 pop rbx
 ret
.real:
 mov rdi,rbx
 pop rbx
 jmp __real_sqlite3_step
section .note.GNU-stack noalloc noexec nowrite progbits
