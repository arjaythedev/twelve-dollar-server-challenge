; SPDX-License-Identifier: MIT
; Optional bounded three-query feed. One owner, request-local scratch only.
section .rodata
range_ids_sql: db 'SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20',0
range_likes_sql: db 'SELECT post_id FROM likes INDEXED BY likes_post_id_idx WHERE post_id BETWEEN ?1 AND ?2',0
range_rows_sql: db 'SELECT p.id,p.body,p.created_at,u.username FROM posts p CROSS JOIN users u WHERE u.id=p.user_id AND p.id BETWEEN ?1 AND ?2 ORDER BY p.id',0
range_queries: dq range_ids_sql,range_likes_sql,range_rows_sql
section .bss
range_stmts: resq 3
range_ids: resq 20
range_counts: resq 20
range_offsets: resd 20
range_lengths: resd 20
range_map: resb 256
range_arena: resb 65536
range_count: resd 1
range_lo: resq 1
range_hi: resq 1
range_own_tx: resd 1
section .text
ranges_prepare:
 ENTER
 xor ebx,ebx
.loop:
 lea rax,[range_queries]
 mov rdi,[rax+rbx*8]
 lea rsi,[range_stmts]
 lea rsi,[rsi+rbx*8]
 call prepare
 test eax,eax
 jnz .exit
 inc ebx
 cmp ebx,3
 jb .loop
.exit: LEAVE
ranges_finalize:
 ENTER
 xor ebx,ebx
.loop:
 lea rax,[range_stmts]
 mov rdi,[rax+rbx*8]
 call sqlite3_finalize
 inc ebx
 cmp ebx,3
 jb .loop
 LEAVE
; Bind exact signed INTEGER bounds to current r12 statement.
ranges_bind:
 sub rsp,8
 mov rdi,r12
 mov esi,1
 mov rdx,[range_lo]
 call sqlite3_bind_int64
 test eax,eax
 jnz .exit
 mov rdi,r12
 mov esi,2
 mov rdx,[range_hi]
 call sqlite3_bind_int64
.exit: add rsp,8
 ret
ranges_feed:
 ENTER
 mov dword [range_own_tx],0
 cmp dword [batch_active],0
 jne .snapshot
 mov edi,6
 call control_step
 test eax,eax
 jnz .error
 mov dword [range_own_tx],1
.snapshot:
 mov dword [range_count],0
 mov qword [range_lo],0x7fffffff
 mov rax,0x7fffffffffffffff
 mov [range_lo],rax
 mov rax,0x8000000000000000
 mov [range_hi],rax
 mov r12,[range_stmts]
.ids:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .ids_done
 cmp eax,100
 jne .reset_error
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_int64
 mov ecx,[range_count]
 cmp ecx,20
 jae .reset_error
 lea rdx,[range_ids]
 mov [rdx+rcx*8],rax
 inc dword [range_count]
 cmp rax,[range_lo]
 jge .not_lo
 mov [range_lo],rax
.not_lo:
 cmp rax,[range_hi]
 jle .ids
 mov [range_hi],rax
 jmp .ids
.ids_done:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .error
 cmp dword [range_count],0
 je .assemble_empty
 mov rax,[range_hi]
 sub rax,[range_lo]
 cmp rax,256
 jae .fallback
 lea rdi,[range_map]
 mov ecx,256
 mov al,255
 rep stosb
 lea rdi,[range_counts]
 xor eax,eax
 mov ecx,20
 rep stosq
 lea rdi,[range_lengths]
 mov ecx,20
 rep stosd
 xor ecx,ecx
.map:
 lea rdx,[range_ids]
 mov rax,[rdx+rcx*8]
 sub rax,[range_lo]
 lea rdx,[range_map]
 mov [rdx+rax],cl
 inc ecx
 cmp ecx,[range_count]
 jb .map
 mov r12,[range_stmts+8]
 call ranges_bind
 test eax,eax
 jnz .reset_error
 xor ebx,ebx
.likes:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .likes_done
 cmp eax,100
 jne .reset_error
 inc ebx
 cmp ebx,4096
 ja .reset_fallback
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_int64
 sub rax,[range_lo]
 cmp rax,256
 jae .reset_error
 lea rdx,[range_map]
 movzx ecx,byte [rdx+rax]
 cmp ecx,255
 je .likes
 lea rdx,[range_counts]
 inc qword [rdx+rcx*8]
 jmp .likes
.likes_done:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .error
 mov r12,[range_stmts+16]
 call ranges_bind
 test eax,eax
 jnz .reset_error
 lea r13,[range_arena]
.rows:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .rows_done
 cmp eax,100
 jne .reset_error
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_int64
 sub rax,[range_lo]
 cmp rax,256
 jae .reset_error
 lea rdx,[range_map]
 movzx ebx,byte [rdx+rax]
 cmp ebx,255
 je .rows
 lea rax,[range_arena]
 mov rdx,r13
 sub rdx,rax
 lea rax,[range_offsets]
 mov [rax+rbx*4],edx
 mov rdi,r13
 mov rsi,r12
 lea rax,[range_counts]
 mov rdx,[rax+rbx*8]
 lea rcx,[range_arena+65000]
 call append_post_counted
 test rax,rax
 js .reset_error
 mov rdx,rax
 sub rdx,r13
 lea rcx,[range_lengths]
 mov [rcx+rbx*4],edx
 mov r13,rax
 jmp .rows
.rows_done:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .error
.assemble_empty:
 lea rdi,[response_body]
 APPEND feed_prefix
 xor ebx,ebx
.assemble:
 cmp ebx,[range_count]
 jae .end_json
 test ebx,ebx
 jz .first
 mov byte [rdi],','
 inc rdi
.first:
 lea rax,[range_lengths]
 mov ecx,[rax+rbx*4]
 test ecx,ecx
 jz .error
 lea rax,[range_offsets]
 mov esi,[rax+rbx*4]
 lea rax,[range_arena]
 add rsi,rax
 rep movsb
 inc ebx
 jmp .assemble
.end_json:
 mov word [rdi],0x7d5d
 add rdi,2
 lea rax,[response_body]
 sub rdi,rax
 mov [response_len],rdi
 jmp .success
.reset_fallback:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .error
.fallback:
 mov r12,[statements+8]
 lea rdi,[response_body]
 APPEND feed_prefix
 mov r13,rdi
 xor ebx,ebx
.fallback_row:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .fallback_done
 cmp eax,100
 jne .reset_error
 mov rdi,r13
 test ebx,ebx
 jz .fallback_first
 mov byte [rdi],','
 inc rdi
.fallback_first:
 mov rsi,r12
 call append_post
 test rax,rax
 js .reset_error
 mov r13,rax
 inc ebx
 jmp .fallback_row
.fallback_done:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .error
 mov rdi,r13
 jmp .end_json
.success:
 cmp dword [range_own_tx],0
 je .ok
 mov edi,7
 call control_step
 test eax,eax
 jnz .error
.ok: xor eax,eax
 jmp .exit
.reset_error:
 mov rdi,r12
 call sqlite3_reset
.error:
 cmp dword [range_own_tx],0
 je .batch_error
 mov edi,8
 call control_step
 jmp .bad
.batch_error:
 cmp dword [batch_active],0
 je .bad
 ; Never acknowledge other tentative writes after unexpected read failure.
 mov dword [batch_failed],1
.bad: mov eax,-1
.exit: LEAVE
