; SPDX-License-Identifier: MIT
; Diagnostic CLI controls do not add runtime environment variables.
section .rodata
LITERAL feed_prefix
LITERAL single_prefix
LITERAL id_prefix
LITERAL likes_prefix
arg_literal_scalar: db '--literal-copy=scalar',0
arg_literal_inline: db '--literal-copy=inline',0
arg_batch_1: db '--batch=1',0
arg_batch_32: db '--batch=32',0
arg_batch_64: db '--batch=64',0
arg_batch_128: db '--batch=128',0
arg_wait_0: db '--wait-us=0',0
arg_wait_250: db '--wait-us=250',0
arg_wait_1000: db '--wait-us=1000',0
arg_wait_4000: db '--wait-us=4000',0
arg_wait_6000: db '--wait-us=6000',0
arg_nodelay_on: db '--nodelay=on',0
arg_nodelay_off: db '--nodelay=off',0
arg_enobuf_defer: db '--enobuf=defer',0
arg_enobuf_immediate: db '--enobuf=immediate',0
arg_send_first: db '--completion-order=send-first',0
arg_fifo: db '--completion-order=fifo',0
arg_recv_one: db '--recv=one',0
arg_recv_multi: db '--recv=multi',0
arg_epoll: db '--epoll',0
arg_request: db '--transaction=per-request',0
arg_writes: db '--transaction=writes',0
arg_batch: db '--transaction=batch',0
arg_one_ring: db '--rings=1',0
arg_query_reference: db '--query=reference',0
arg_query_selected: db '--query=selected',0
arg_query_range: db '--query=range',0
arg_query_scan: db '--query=scan-ranges',0
arg_savepoints_on: db '--savepoints=on',0
arg_savepoints_off: db '--savepoints=off',0
arg_quote_scalar: db '--quote=scalar',0
arg_quote_vector: db '--quote=vector',0
arg_json_sql: db '--json=sql',0
arg_json_nasm: db '--json=nasm',0
arg_json_hybrid: db '--json=hybrid',0
arg_eight_rings: db '--rings=8',0
section .data
literal_copy_inline: dd 0
requested_rings: dd 8
transaction_mode: dd 2
completion_target: dd 32
wait_us: dd 250
nodelay: dd 0
recv_mode: dd 0
completion_order: dd 1
enobuf_mode: dd 1
query_mode: dd 0
json_mode: dd 1
savepoints_enabled: dd 0
section .bss
write_error: resd 1
group_context: resd 1
batch_active: resd 1
batch_failed: resd 1
reply_count: resd 1
deferred_count: resd 1
batch_replies: resd 256
deferred_writes: resd 256
section .text
arg_equal:
.loop:
 mov al,[rdi]
 cmp al,[rsi]
 jne .no
 inc rdi
 inc rsi
 test al,al
 jnz .loop
 mov eax,1
 ret
.no: xor eax,eax
 ret
parse_cli:
 ENTER
 mov r14d,edi
 mov r15,rsi
 mov ebx,1
.loop:
 cmp ebx,r14d
 jae .done
 mov r13,[r15+rbx*8]
 lea rbp,[option_table]
.option:
 mov rsi,[rbp]
 test rsi,rsi
 jz fatal
 mov rdi,r13
 call arg_equal
 test eax,eax
 jnz .found
 add rbp,24
 jmp .option
.found:
 mov rax,[rbp+8]
 mov ecx,[rbp+16]
 mov [rax],ecx
 inc ebx
 jmp .loop
.done: LEAVE
section .data
option_table:
 dq arg_literal_scalar,literal_copy_inline,0
 dq arg_literal_inline,literal_copy_inline,1
 dq arg_epoll,force_epoll,1
 dq arg_request,transaction_mode,0
 dq arg_writes,transaction_mode,1
 dq arg_batch,transaction_mode,2
 dq arg_one_ring,requested_rings,1
 dq arg_eight_rings,requested_rings,8
 dq arg_query_reference,query_mode,0
 dq arg_query_selected,query_mode,1
 dq arg_query_range,query_mode,2
 dq arg_query_scan,query_mode,3
 dq arg_savepoints_on,savepoints_enabled,1
 dq arg_savepoints_off,savepoints_enabled,0
 dq arg_quote_scalar,quote_vector_enabled,0
 dq arg_quote_vector,quote_vector_enabled,1
 dq arg_json_sql,json_mode,0
 dq arg_json_nasm,json_mode,1
 dq arg_json_hybrid,json_mode,2
 dq arg_batch_1,completion_target,1
 dq arg_batch_32,completion_target,32
 dq arg_batch_64,completion_target,64
 dq arg_batch_128,completion_target,128
 dq arg_wait_0,wait_us,0
 dq arg_wait_250,wait_us,250
 dq arg_wait_1000,wait_us,1000
 dq arg_wait_4000,wait_us,4000
 dq arg_wait_6000,wait_us,6000
 dq arg_nodelay_on,nodelay,1
 dq arg_nodelay_off,nodelay,0
 dq arg_enobuf_defer,enobuf_mode,1
 dq arg_enobuf_immediate,enobuf_mode,0
 dq arg_send_first,completion_order,1
 dq arg_fifo,completion_order,0
 dq arg_recv_one,recv_mode,0
 dq arg_recv_multi,recv_mode,1
 dq 0,0,0
section .text
; Select SQL only during initialization; all used statements prepared once.
select_query:
 cmp ebx,1
 je .feed
 cmp ebx,2
 jne .done
 cmp dword [json_mode],0
 je .done
 lea rdi,[q_post_plain]
 ret
.feed:
 cmp dword [query_mode],3
 je .plain
 cmp dword [query_mode],1
 je .selected
 cmp dword [query_mode],2
 je .range
 cmp dword [json_mode],1
 jne .done
.plain:
 lea rdi,[q_feed_plain]
 ret
.selected:
 lea rdi,[q_feed_selected_json]
 cmp dword [json_mode],1
 jne .done
 lea rdi,[q_feed_selected_plain]
 ret
.range:
 lea rdi,[q_feed_range_json]
 cmp dword [json_mode],1
 jne .done
 lea rdi,[q_feed_range_plain]
.done: ret
batch_begin:
 cmp dword [batch_active],0
 jne .already
 sub rsp,8
 mov edi,6
 call control_step
 test eax,eax
 jnz .failed
 mov dword [batch_active],1
.failed:
 add rsp,8
 ret
.already:
 cmp dword [batch_failed],0
 jne .bad
 xor eax,eax
 ret
.bad:
 mov eax,-1
 ret
write_begin:
 mov dword [write_error],-1
 cmp dword [batch_active],0
 jne .savepoint
 mov edi,6
 jmp control_step
.savepoint:
 cmp dword [savepoints_enabled],0
 je .ok
 mov edi,10
 jmp control_step
.ok: xor eax,eax
 ret
write_commit:
 cmp dword [batch_active],0
 jne .release
 mov edi,7
 jmp control_step
.release:
 cmp dword [savepoints_enabled],0
 je .ok
 mov edi,11
 jmp control_step
.ok: xor eax,eax
 ret
write_rollback:
 ENTER
 cmp dword [batch_active],0
 jne .savepoint
 mov edi,8
 call control_step
 jmp .exit
.savepoint:
 cmp dword [savepoints_enabled],0
 jne .explicit_savepoint
 ; Fixed write SQL uses ABORT for constraints; only that error is recoverable.
 mov eax,[write_error]
 and eax,255
 cmp eax,19
 jne .failed
 mov rdi,[database]
 call sqlite3_get_autocommit
 test eax,eax
 jnz .failed
 jmp .exit
.explicit_savepoint:
 mov rdi,[database]
 call sqlite3_get_autocommit
 test eax,eax
 jnz .failed
 mov edi,12
 call control_step
 test eax,eax
 jnz .failed
 mov edi,11
 call control_step
 test eax,eax
 jnz .failed
 jmp .exit
.failed:
 mov dword [batch_failed],1
 mov edi,8
 call control_step
.exit: LEAVE
flush_batch:
 ENTER
 cmp dword [deferred_count],0
 je .commit
 call batch_begin
 test eax,eax
 jz .begin_ok
 mov dword [batch_active],1
 mov dword [batch_failed],1
.begin_ok:
 xor ebx,ebx
.writes:
 cmp ebx,[deferred_count]
 jae .commit
 lea rax,[deferred_writes]
 mov ebp,[rax+rbx*4]
 mov r12d,ebp
 shl r12,6
 lea rax,[connections]
 add r12,rax
 mov rdi,r12
 call parse_http
 cmp eax,1
 jne fatal
 mov edi,ebp
 call uring_finish_request
 inc ebx
 jmp .writes
.commit:
 cmp dword [batch_active],0
 je .send
 cmp dword [batch_failed],0
 jne .fail
 mov edi,7
 call control_step
 test eax,eax
 jz .send
.fail:
 mov dword [batch_failed],1
 mov edi,8
 call control_step
.send:
 mov dword [batch_active],0
 mov dword [group_context],0
 xor ebx,ebx
.replies:
 cmp ebx,[reply_count]
 jae .done
 lea rax,[batch_replies]
 mov ebp,[rax+rbx*4]
 cmp dword [batch_failed],0
 je .submit
 mov r12d,ebp
 shl r12,6
 lea rax,[connections]
 add r12,rax
 lea rdi,[response_body]
 lea rsi,[err_internal]
 call append_z
 lea rax,[response_body]
 sub rdi,rax
 mov [response_len],rdi
 mov dword [response_status],500
 mov rdi,r12
 call build_response
.submit:
 mov edi,ebp
 call uring_send
 inc ebx
 jmp .replies
.done:
 mov dword [reply_count],0
 mov dword [deferred_count],0
 call maybe_checkpoint
 LEAVE
; rdi destination, rsi statement. Copy each SQLite text before advancing.
append_post:
 mov rdx,-1
 lea rcx,[response_body+65000]
append_post_counted:
 ENTER
 mov r15,rdx
 mov rbp,rcx
 mov r12,rsi
 APPEND id_prefix
 mov r13,rdi
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_int64
 mov rdi,r13
 test rax,rax
 jns .positive_id
 mov byte [rdi],45
 inc rdi
 neg rax
.positive_id:
 call append_uint
 APPEND body_prefix
 mov esi,1
 call .text
 test rax,rax
 js .bad
 mov rdi,rax
 APPEND created_prefix
 mov esi,2
 call .text
 test rax,rax
 js .bad
 mov rdi,rax
 APPEND author_prefix
 mov esi,3
 call .text
 test rax,rax
 js .bad
 mov rdi,rax
 APPEND likes_prefix
 mov r13,rdi
 mov rdi,r12
 mov rax,r15
 test rax,rax
 jns .count_ready
 mov esi,4
 call sqlite3_column_int64
.count_ready:
 mov rdi,r13
 call append_uint
 mov byte [rdi],'}'
 lea rax,[rdi+1]
 jmp .exit
.bad: mov rax,-1
.exit: LEAVE
.text:
 sub rsp,8
 mov r13,rdi
 mov ebx,esi
 mov rdi,r12
 call sqlite3_column_text
 test rax,rax
 jz .terr
 mov r14,rax
 mov rdi,r12
 mov esi,ebx
 call sqlite3_column_bytes
 mov edx,eax
 imul rax,rax,6
 lea rax,[rax+r13+64]
 cmp rax,rbp
 ja .terr
 mov rdi,r13
 mov rsi,r14
 call json_quote
 add rsp,8
 ret
.terr:
 mov rax,-1
 add rsp,8
 ret
