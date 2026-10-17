; SPDX-License-Identifier: MIT
; Diagnostic only: links the same SQLite object as the server, never the server.
; query-profile DATABASE SQL ITERATIONS; all result text copied before reset.
default rel
global main
extern sqlite3_config, sqlite3_open_v2, sqlite3_prepare_v2, sqlite3_step, sqlite3_exec
extern sqlite3_reset, sqlite3_finalize, sqlite3_close, sqlite3_stmt_status
extern sqlite3_column_count, sqlite3_column_text, sqlite3_column_bytes
extern sqlite3_status64, atoi, printf
section .rodata
settings: db "PRAGMA mmap_size=1073741824; PRAGMA cache_size=500; PRAGMA temp_store=MEMORY;",0
fmt: db '{"iterations":%d,"cpu_ns":%ld,"vm_steps":%d,"fullscan_steps":%d,"sorts":%d,"autoindex_rows":%d,"statement_bytes":%d,"memory_highwater":%ld,"malloc_highwater":%ld,"steps":%ld,"column_api_calls":%ld,"result_bytes":%ld,"allocation_calls":%ld,"reallocation_calls":%ld}',10,0
section .bss
handle: resq 1
stmt: resq 1
current: resq 1
high: resq 1
memhigh: resq 1
mallochigh: resq 1
start: resq 2
finish: resq 2
steps: resq 1
columns: resq 1
bytes: resq 1
original_methods: resq 8
profile_methods: resq 8
allocations: resq 1
reallocations: resq 1
buffer: resb 65536
section .text
main:
 push rbp
 push rbx
 push r12
 push r13
 push r14
 push r15
 sub rsp,8
 cmp edi,4
 jne .fail
 mov r12,rsi
 mov rdi,[r12+24]
 call atoi
 test eax,eax
 jle .fail
 mov r13d,eax
 mov edi,5 ; SQLITE_CONFIG_GETMALLOC
 lea rsi,[original_methods]
 xor eax,eax
 call sqlite3_config
 test eax,eax
 jnz .fail
 lea rsi,[original_methods]
 lea rdi,[profile_methods]
 mov ecx,8
 rep movsq
 lea rax,[count_malloc]
 mov [profile_methods],rax
 lea rax,[count_realloc]
 mov [profile_methods+16],rax
 mov edi,4 ; SQLITE_CONFIG_MALLOC
 lea rsi,[profile_methods]
 xor eax,eax
 call sqlite3_config
 test eax,eax
 jnz .fail
 mov edi,9 ; SQLITE_CONFIG_MEMSTATUS, diagnostic counters only
 mov esi,1
 xor eax,eax
 call sqlite3_config
 test eax,eax
 jnz .fail
 mov rdi,[r12+8]
 lea rsi,[handle]
 mov edx,1 ; readonly
 xor ecx,ecx
 call sqlite3_open_v2
 test eax,eax
 jnz .fail
 mov rdi,[handle]
 lea rsi,[settings]
 xor edx,edx
 xor ecx,ecx
 xor r8d,r8d
 call sqlite3_exec
 test eax,eax
 jnz .fail
 mov rdi,[handle]
 mov rsi,[r12+16]
 mov edx,-1
 lea rcx,[stmt]
 xor r8d,r8d
 call sqlite3_prepare_v2
 test eax,eax
 jnz .fail
 mov edi,0
 lea rsi,[current]
 lea rdx,[high]
 mov ecx,1
 call sqlite3_status64
 mov edi,9
 lea rsi,[current]
 lea rdx,[high]
 mov ecx,1
 call sqlite3_status64
 mov qword [allocations],0
 mov qword [reallocations],0
 mov eax,228
 mov edi,2 ; CLOCK_PROCESS_CPUTIME_ID
 lea rsi,[start]
 syscall
 xor r14d,r14d
.iteration:
 mov rdi,[stmt]
 call sqlite3_step
 inc qword [steps]
 cmp eax,101
 je .reset
 cmp eax,100
 jne .fail
 mov rdi,[stmt]
 call sqlite3_column_count
 inc qword [columns]
 mov r15d,eax
 xor ebx,ebx
 xor ebp,ebp
.column:
 cmp ebx,r15d
 jae .iteration
 mov rdi,[stmt]
 mov esi,ebx
 call sqlite3_column_text
 inc qword [columns]
 mov r12,rax
 mov rdi,[stmt]
 mov esi,ebx
 call sqlite3_column_bytes
 inc qword [columns]
 mov ecx,eax
 add rbp,rcx
 cmp rbp,65536
 ja .fail
 add [bytes],rcx
 lea rdi,[buffer]
 add rdi,rbp
 sub rdi,rcx
 mov rsi,r12
 rep movsb
 inc ebx
 jmp .column
.reset:
 mov rdi,[stmt]
 call sqlite3_reset
 test eax,eax
 jnz .fail
 inc r14d
 cmp r14d,r13d
 jb .iteration
 mov eax,228
 mov edi,2
 lea rsi,[finish]
 syscall
 ; Save variadic argument values in an aligned stack area.
 sub rsp,80
 mov rdi,[stmt]
 mov esi,99
 xor edx,edx
 call sqlite3_stmt_status
 mov [rsp+8],rax
 mov rdi,[stmt]
 mov esi,3
 xor edx,edx
 call sqlite3_stmt_status
 mov [rsp],rax
 mov rdi,[stmt]
 mov esi,2
 xor edx,edx
 call sqlite3_stmt_status
 mov r15d,eax
 mov rdi,[stmt]
 mov esi,1
 xor edx,edx
 call sqlite3_stmt_status
 mov r14d,eax
 mov rdi,[stmt]
 mov esi,4
 xor edx,edx
 call sqlite3_stmt_status
 mov ebx,eax
 mov edi,0
 lea rsi,[current]
 lea rdx,[memhigh]
 xor ecx,ecx
 call sqlite3_status64
 mov edi,9
 lea rsi,[current]
 lea rdx,[mallochigh]
 xor ecx,ecx
 call sqlite3_status64
 mov rax,[memhigh]
 mov [rsp+16],rax
 mov rax,[mallochigh]
 mov [rsp+24],rax
 mov rax,[steps]
 mov [rsp+32],rax
 mov rax,[columns]
 mov [rsp+40],rax
 mov rax,[bytes]
 mov [rsp+48],rax
 mov rax,[allocations]
 mov [rsp+56],rax
 mov rax,[reallocations]
 mov [rsp+64],rax
 mov rdx,[finish]
 sub rdx,[start]
 imul rdx,1000000000
 add rdx,[finish+8]
 sub rdx,[start+8]
 lea rdi,[fmt]
 mov esi,r13d
 mov ecx,ebx
 mov r8d,r14d
 mov r9d,r15d
 xor eax,eax
 call printf
 add rsp,80
 mov rdi,[stmt]
 call sqlite3_finalize
 mov rdi,[handle]
 call sqlite3_close
 xor eax,eax
 jmp .done
.fail:
 mov eax,1
.done:
 add rsp,8
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbx
 pop rbp
 ret
count_malloc:
 inc qword [allocations]
 jmp [original_methods]
count_realloc:
 inc qword [reallocations]
 jmp [original_methods+16]
section .note.GNU-stack noalloc noexec nowrite progbits
