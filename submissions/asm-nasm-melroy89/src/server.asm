; SPDX-License-Identifier: MIT
; Direct HTTP Linux x86-64. Application NASM, SQLite public C ABI.
default rel
%define IN_CAP 32768
%define OUT_CAP 65536
%define MAX_FD 524288
%macro ENTER 0
 push rbp
 push rbx
 push r12
 push r13
 push r14
 push r15
 sub rsp,8
%endmacro
%macro LEAVE 0
 add rsp,8
 pop r15
 pop r14
 pop r13
 pop r12
 pop rbx
 pop rbp
 ret
%endmacro
%include "literals.inc"
extern sqlite3_open_v2,sqlite3_prepare_v3,sqlite3_step,sqlite3_reset,sqlite3_finalize
extern sqlite3_column_text,sqlite3_column_bytes,sqlite3_column_int64,sqlite3_bind_int64,sqlite3_bind_text
extern sqlite3_close,sqlite3_errmsg,sqlite3_exec,sqlite3_changes,sqlite3_get_autocommit,sqlite3_wal_hook,sqlite3_wal_checkpoint_v2
extern numeric_date,quote_vector_enabled
extern sha256,hmac_init,hmac_sha256,b64decode,json_parse,json_fields,trim_utf8,json_quote,parse_uint,str_len,bytes_equal
section .rodata
%include "sql.inc"
config_names: dq env_db,env_secret,env_host,env_port
env_db: db 'SQLITE_PATH=',0
env_secret: db 'JWT_SECRET=',0
env_host: db 'HOST=',0
env_port: db 'PORT=',0
fatal_text: db 'NASM startup failed: check environment, SQLite and bind permissions.',10
fatal_len equ $-fatal_text
ready_text: db 'NASM direct HTTP ready (epoll, WAL/NORMAL, foreign keys ON).',10
ready_len equ $-ready_text
path_health: db '/health',0
path_feed: db '/feed',0
path_posts: db '/posts',0
prefix_posts: db '/posts/',0
suffix_like: db '/like',0
header_auth: db 'authorization',0
header_length: db 'content-length',0
header_transfer: db 'transfer-encoding',0
header_expect: db 'expect',0
header_connection: db 'connection',0
bearer: db 'Bearer ',0
hs256: db 'HS256',0
chunked_word: db 'chunked',0
continue_word: db '100-continue',0
close_word: db 'close',0
http10: db 'HTTP/1.0',0
http11: db 'HTTP/1.1',0
interim: db 'HTTP/1.1 100 Continue',13,10,13,10
interim_len equ $-interim
status_200: db 'HTTP/1.1 200 OK',13,10,0
status_201: db 'HTTP/1.1 201 Created',13,10,0
status_400: db 'HTTP/1.1 400 Bad Request',13,10,0
status_401: db 'HTTP/1.1 401 Unauthorized',13,10,0
status_404: db 'HTTP/1.1 404 Not Found',13,10,0
status_413: db 'HTTP/1.1 413 Payload Too Large',13,10,0
status_500: db 'HTTP/1.1 500 Internal Server Error',13,10,0
status_503: db 'HTTP/1.1 503 Service Unavailable',13,10,0
LITERAL content_header
keep_header: db 13,10,'Connection: keep-alive',13,10,13,10,0
close_header: db 13,10,'Connection: close',13,10,13,10,0
err_notfound: db '{"error":"not found"}',0
err_id: db '{"error":"invalid post id"}',0
err_post: db '{"error":"post not found"}',0
err_bearer: db '{"error":"missing bearer token"}',0
err_token: db '{"error":"invalid or expired token"}',0
err_payload: db '{"error":"invalid token payload"}',0
err_json: db '{"error":"malformed JSON body"}',0
err_body: db '{"error":"body is required"}',0
err_long: db '{"error":"body must be at most 500 characters"}',0
err_internal: db '{"error":"internal server error"}',0
err_busy: db '{"error":"server busy"}',0
err_health: db '{"status":"degraded","db":"unreachable","error":"SQLite SELECT 1 failed"}',0
LITERAL health_prefix
LITERAL post_prefix
LITERAL body_prefix
LITERAL created_prefix
LITERAL author_prefix
LITERAL post_end
like_first: db '{"liked":true,"already_liked":false,"post_id":',0
like_repeat: db '{"liked":true,"already_liked":true,"post_id":',0
section .data
align 8
signal_action: dq signal_stop,0x04000000,signal_restorer,0
one: dd 1
section .bss
align 64
configs: resq 4
database: resq 1
statements: resq 13
check_stmt: resq 1
listen_fd: resd 1
epoll_fd: resd 1
sockaddr: resb 16
ctl_event: resb 12
events: resb 3072
connections: resb MAX_FD*64
generations: resd MAX_FD
timers: resd 256
input_free: resq 1
output_free: resq 1
input_bump: resq 1
output_bump: resq 1
alignb 4096
input_pool: resb 32*1024*1024
output_pool: resb 64*1024*1024
now_sec: resq 1
boot_sec: resq 1
last_tick: resq 1
clock_buf: resq 2
stop_flag: resd 1
input_allocated: resq 1
output_allocated: resq 1
active_count: resq 1
request_count: resq 1
wal_frames: resd 1
wal_log: resd 1
wal_checkpointed: resd 1
method: resd 1
route: resd 1
path_ptr: resq 1
path_len: resq 1
auth_ptr: resq 1
auth_len: resq 1
body_ptr: resq 1
body_len: resq 1
request_bytes: resq 1
header_bytes: resq 1
content_length: resq 1
transfer_seen: resd 1
length_seen: resd 1
expect_seen: resd 1
request_close: resd 1
response_status: resd 1
user_id: resq 1
post_id: resq 1
user_name_len: resq 1
user_name: resb 8192
jwt_header: resb 8192
jwt_payload: resb 8192
jwt_sig: resb 64
jwt_expected: resb 32
new_created: resb 64
post_text: resb 16384
post_text_len: resq 1
chunk_body: resb 16384
response_body: resb 65536
response_len: resq 1
response_cursor: resq 1
section .text
global main
main:
 ENTER
 mov r12,rdx
 call parse_cli
.env:
 mov r13,[r12]
 test r13,r13
 jz .configured
 xor ebx,ebx
.name:
 lea rax,[config_names]
 mov rsi,[rax+rbx*8]
 mov rdi,r13
.match:
 mov al,[rsi]
 test al,al
 jz .found
 cmp al,[rdi]
 jne .next
 inc rdi
 inc rsi
 jmp .match
.found:
 lea rax,[configs]
 mov [rax+rbx*8],rdi
.next:
 inc ebx
 cmp ebx,4
 jb .name
 add r12,8
 jmp .env
.configured:
 lea r12,[configs]
 mov ecx,4
.checkenv:
 cmp qword [r12],0
 je fatal
 mov rax,[r12]
 cmp byte [rax],0
 je fatal
 add r12,8
 loop .checkenv
 mov rdi,[configs+8]
 call str_len
 mov rsi,rax
 mov rdi,[configs+8]
 call hmac_init
 mov eax,13
 mov edi,15
 lea rsi,[signal_action]
 xor edx,edx
 mov r10d,8
 syscall
 mov eax,13
 mov edi,2
 lea rsi,[signal_action]
 xor edx,edx
 mov r10d,8
 syscall
 call db_init
 test eax,eax
 jnz fatal
 call clock_update
 mov rax,[now_sec]
 mov [boot_sec],rax
 mov [last_tick],rax
 mov rsi,[configs+16]
 lea rdi,[sockaddr+4]
 mov ebx,4
.ip_octet:
 xor eax,eax
 xor ecx,ecx
.ip_digit:
 movzx edx,byte [rsi]
 cmp dl,'0'
 jb .ip_end
 cmp dl,'9'
 ja .ip_end
 imul eax,eax,10
 sub edx,'0'
 add eax,edx
 cmp eax,255
 ja fatal
 inc rsi
 inc ecx
 jmp .ip_digit
.ip_end:
 test ecx,ecx
 jz fatal
 mov [rdi],al
 inc rdi
 dec ebx
 jz .ip_last
 cmp byte [rsi],'.'
 jne fatal
 inc rsi
 jmp .ip_octet
.ip_last:
 cmp byte [rsi],0
 jne fatal
 mov rdi,[configs+24]
 call str_len
 mov rsi,rax
 mov rdi,[configs+24]
 call parse_uint
 cmp rax,65535
 ja fatal
 rol ax,8
 mov [sockaddr+2],ax
 mov word [sockaddr],2
 mov eax,41
 mov edi,2
 mov esi,0x80801
 xor edx,edx
 syscall
 test eax,eax
 js fatal
 mov [listen_fd],eax
 mov edi,eax
 mov eax,54
 mov esi,1
 mov edx,2
 lea r10,[one]
 mov r8d,4
 syscall
 mov eax,54
 mov edi,[listen_fd]
 mov esi,6
 mov edx,1
 lea r10,[nodelay]
 mov r8d,4
 syscall
 mov eax,49
 mov edi,[listen_fd]
 lea rsi,[sockaddr]
 mov edx,16
 syscall
 test eax,eax
 js fatal
 mov eax,50
 mov edi,[listen_fd]
 mov esi,4096
 syscall
 test eax,eax
 js fatal
 mov eax,291
 mov edi,0x80000
 syscall
 test eax,eax
 js fatal
 mov [epoll_fd],eax
 cmp dword [force_epoll],0
 jne .epoll_listen
 call uring_init
 test eax,eax
 jnz .epoll_listen
 mov eax,1
 mov edi,2
 lea rsi,[uring_ready]
 mov edx,uring_ready_len
 syscall
 call uring_loop
 jmp .shutdown
.epoll_listen:
 mov dword [ctl_event],1
 mov qword [ctl_event+4],-1
 mov eax,233
 mov edi,[epoll_fd]
 mov esi,1
 mov edx,[listen_fd]
 lea r10,[ctl_event]
 syscall
 test eax,eax
 js fatal
 mov eax,1
 mov edi,2
 lea rsi,[ready_text]
 mov edx,ready_len
 syscall
.loop:
 cmp dword [stop_flag],0
 jne .shutdown
 mov eax,232
 mov edi,[epoll_fd]
 lea rsi,[events]
 mov edx,256
 mov r10d,1000
 syscall
 cmp eax,-4
 je .loop
 test eax,eax
 js .shutdown
 mov r14d,eax
 xor r15d,r15d
 call clock_update
 call expire_timers
.event:
 cmp r15d,r14d
 jae .loop
 mov eax,r15d
 imul eax,eax,12
 lea rbx,[events]
 add rbx,rax
 mov rdi,[rbx+4]
 cmp rdi,-1
 je .accept
 mov esi,[rbx]
 call connection_event
 jmp .next_event
.accept:
 call accept_connections
.next_event:
 inc r15d
 jmp .event
.shutdown:
 cmp dword [uring_enabled],0
 je .epoll_shutdown
 call uring_shutdown
 jmp .close_listener
.epoll_shutdown:
 xor r12d,r12d
.close_loop:
 mov edi,r12d
 call close_connection
 inc r12d
 cmp r12d,MAX_FD
 jb .close_loop
.close_listener:
 mov eax,3
 mov edi,[listen_fd]
 syscall
 xor ebx,ebx
.finalize:
 lea rax,[statements]
 mov rdi,[rax+rbx*8]
 call sqlite3_finalize
 inc ebx
 cmp ebx,13
 jb .finalize
 call ranges_finalize
 mov rdi,[database]
 call sqlite3_close
 xor eax,eax
 LEAVE
fatal:
 mov eax,1
 mov edi,2
 lea rsi,[fatal_text]
 mov edx,fatal_len
 syscall
 mov eax,231
 mov edi,1
 syscall
signal_stop:
 mov dword [stop_flag],1
 ret
signal_restorer:
 mov eax,15
 syscall
clock_update:
 mov eax,228
 mov edi,1
 lea rsi,[clock_buf]
 syscall
 mov rax,[clock_buf]
 mov [now_sec],rax
 ret
prepare:
 mov rcx,rsi
 mov rsi,rdi
 mov rdi,[database]
 mov edx,-1
 mov r8,rcx
 mov ecx,1
 xor r9d,r9d
 jmp sqlite3_prepare_v3
db_init:
 ENTER
 mov rdi,[configs]
 lea rsi,[database]
 mov edx,0x8002
 xor ecx,ecx
 call sqlite3_open_v2
 test eax,eax
 jnz .exit
 mov rdi,[database]
 lea rsi,[settings]
 xor edx,edx
 xor ecx,ecx
 xor r8d,r8d
 call sqlite3_exec
 test eax,eax
 jnz .exit
 lea rdi,[q_wal]
 lea rsi,[check_stmt]
 call prepare
 test eax,eax
 jnz .exit
 mov rdi,[check_stmt]
 call sqlite3_step
 cmp eax,100
 jne .fail
 mov rdi,[check_stmt]
 xor esi,esi
 call sqlite3_column_text
 test rax,rax
 jz .fail
 cmp dword [rax],0x006c6177
 jne .fail
 mov rdi,[check_stmt]
 call sqlite3_finalize
 lea rdi,[q_sync]
 call .verify_one
 test eax,eax
 jnz .exit
 lea rdi,[q_fk]
 call .verify_one
 test eax,eax
 jnz .exit
 xor ebx,ebx
.prep:
 lea rax,[queries]
 mov rdi,[rax+rbx*8]
 call select_query
 lea rsi,[statements]
 lea rsi,[rsi+rbx*8]
 call prepare
 test eax,eax
 jnz .exit
 inc ebx
 cmp ebx,13
 jb .prep
 call ranges_prepare
 test eax,eax
 jnz .exit
 mov rdi,[database]
 lea rsi,[wal_hook]
 xor edx,edx
 call sqlite3_wal_hook
 xor eax,eax
.exit: LEAVE
.fail: mov eax,-1
 jmp .exit
.verify_one:
 sub rsp,8
 lea rsi,[check_stmt]
 call prepare
 test eax,eax
 jnz .vexit
 mov rdi,[check_stmt]
 call sqlite3_step
 cmp eax,100
 jne .vfail
 mov rdi,[check_stmt]
 xor esi,esi
 call sqlite3_column_int64
 cmp rax,1
 jne .vfail
 mov rdi,[check_stmt]
 call sqlite3_finalize
 xor eax,eax
.vexit:
 add rsp,8
 ret
.vfail: mov eax,-1
 jmp .vexit
timer_remove:
 lea r8,[connections]
 mov eax,edi
 shl rax,6
 add r8,rax
 mov eax,[r8+48]
 mov ecx,[r8+52]
 test ecx,ecx
 jz .head
 dec ecx
 shl rcx,6
 lea r9,[connections]
 mov [r9+rcx+48],eax
 jmp .next
.head:
 mov ecx,[r8+56]
 lea r9,[timers]
 mov [r9+rcx*4],eax
.next:
 test eax,eax
 jz .done
 dec eax
 shl rax,6
 lea r9,[connections]
 mov ecx,[r8+52]
 mov [r9+rax+52],ecx
.done:
 mov qword [r8+48],0
 ret
timer_add:
 mov eax,edi
 shl rax,6
 lea r8,[connections]
 add r8,rax
 mov rax,[now_sec]
 add rax,rsi
 mov [r8+40],rax
 and eax,255
 mov [r8+56],eax
 lea r9,[timers]
 mov ecx,[r9+rax*4]
 mov [r8+48],ecx
 lea edx,[rdi+1]
 mov [r9+rax*4],edx
 test ecx,ecx
 jz .done
 dec ecx
 shl rcx,6
 lea r9,[connections]
 mov [r9+rcx+52],edx
.done: ret
timer_touch:
 ENTER
 mov ebx,edi
 call timer_remove
 mov edi,ebx
 mov esi,120
 cmp qword [r8],0
 jne .busy
 cmp qword [r8+16],0
 je .add
.busy: mov esi,30
.add: call timer_add
 LEAVE
expire_timers:
 ENTER
.tick:
 mov rax,[last_tick]
 cmp rax,[now_sec]
 jae .done
 inc rax
 mov [last_tick],rax
 and eax,255
 mov ebx,eax
.bucket:
 lea rax,[timers]
 mov edi,[rax+rbx*4]
 test edi,edi
 jz .tick
 dec edi
 mov eax,edi
 shl rax,6
 lea rdx,[connections]
 add rdx,rax
 mov rax,[rdx+40]
 cmp rax,[now_sec]
 ja .tick
 call close_connection
 jmp .bucket
.done: LEAVE
map_memory:
 mov eax,9
 xor edi,edi
 mov edx,3
 mov r10d,0x22
 mov r8,-1
 xor r9d,r9d
 syscall
 ret
close_connection:
 cmp dword [uring_enabled],0
 jne uring_close
 ENTER
 mov ebx,edi
 cmp ebx,MAX_FD
 jae .done
 mov r12d,ebx
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],1
 jz .done
 call timer_remove
 mov eax,3
 mov edi,ebx
 syscall
 mov rdi,[r12]
 test rdi,rdi
 jz .out
 call release_input
 sub qword [input_allocated],IN_CAP
.out:
 mov rdi,[r12+16]
 test rdi,rdi
 jz .clear
 call release_output
 sub qword [output_allocated],OUT_CAP
.clear:
 mov rdi,r12
 xor eax,eax
 mov ecx,8
 rep stosq
 dec qword [active_count]
.done: LEAVE
accept_connections:
 ENTER
 mov ebx,64
.loop:
 cmp qword [input_allocated],28*1024*1024
 jae .done
 cmp qword [output_allocated],64*1024*1024
 jae .done
 mov eax,288
 mov edi,[listen_fd]
 xor esi,esi
 xor edx,edx
 mov r10d,0x80800
 syscall
 test eax,eax
 js .done
 cmp eax,MAX_FD
 jae .reject
 mov r13d,eax
 mov r12d,eax
 shl r12,6
 lea rax,[connections]
 add r12,rax
 mov dword [r12+60],1
 inc qword [active_count]
 mov edi,r13d
 mov esi,120
 call timer_add
 mov dword [ctl_event],0x2001
 lea rax,[generations]
 inc dword [rax+r13*4]
 mov eax,[rax+r13*4]
 shl rax,32
 or rax,r13
 mov [ctl_event+4],rax
 mov eax,233
 mov edi,[epoll_fd]
 mov esi,1
 mov edx,r13d
 lea r10,[ctl_event]
 syscall
 test eax,eax
 jns .accepted
 mov edi,r13d
 call close_connection
.accepted:
 dec ebx
 jnz .loop
 jmp .done
.reject:
 mov edi,eax
 mov eax,3
 syscall
 jmp .accepted
.done: LEAVE
; ASCII header/token comparison, case insensitive, exact source length.
ascii_equal:
 xor eax,eax
 xor ecx,ecx
.loop:
 cmp rcx,rdx
 jae .end
 mov r8b,[rdi+rcx]
 cmp r8b,'A'
 jb .lower
 cmp r8b,'Z'
 ja .lower
 or r8b,32
.lower:
 cmp r8b,[rsi+rcx]
 jne .done
 inc rcx
 jmp .loop
.end:
 cmp byte [rsi+rcx],0
 sete al
.done: ret
; parse_http(connection record): 1 complete, 0 incomplete, -1 invalid, -2 too large.
parse_http:
 ENTER
 mov rbp,rdi
 mov r12,[rdi]
 mov r13,[rdi+8]
 lea r13,[r12+r13]
 mov qword [auth_ptr],0
 mov qword [auth_len],0
 mov qword [content_length],0
 mov dword [length_seen],0
 mov dword [transfer_seen],0
 mov dword [expect_seen],0
 mov dword [request_close],0
 mov rbx,r12
 ; Locate complete headers before inspecting any fixed-size request-line fields.
.findhead:
 lea rax,[rbx+4]
 cmp rax,r13
 ja .incomplete_header
 cmp dword [rbx],0x0a0d0a0d
 je .headfound
 inc rbx
 mov rax,rbx
 sub rax,r12
 cmp rax,16384
 jae .large
 jmp .findhead
.headfound:
 lea r14,[rbx+2] ; final empty line starts here
 lea rax,[rbx+4]
 sub rax,r12
 mov [header_bytes],rax
 mov dword [method],0
 cmp dword [r12],0x20544547
 je .get
 cmp dword [r12],0x54534f50
 jne .other_method
 cmp byte [r12+4],' '
 jne .bad
 mov dword [method],2
 lea r15,[r12+5]
 jmp .path
.get:
 mov dword [method],1
 lea r15,[r12+4]
 jmp .path
.other_method:
 mov r15,r12
.om:
 cmp r15,r14
 jae .bad
 cmp byte [r15],' '
 je .omdone
 inc r15
 jmp .om
.omdone: inc r15
.path:
 mov [path_ptr],r15
 mov rbx,r15
.pathend:
 cmp rbx,r14
 jae .bad
 cmp byte [rbx],' '
 je .version
 cmp byte [rbx],32
 jb .bad
 inc rbx
 jmp .pathend
.version:
 mov rax,rbx
 sub rax,r15
 test rax,rax
 jz .bad
 mov [path_len],rax
 lea rax,[rbx+11]
 cmp rax,r13
 ja .bad
 mov rax,0x312e312f50545448 ; HTTP/1.1 little endian
 cmp [rbx+1],rax
 je .lineend
 mov rax,0x302e312f50545448
 cmp [rbx+1],rax
 jne .bad
 mov dword [request_close],1
.lineend:
 cmp word [rbx+9],0x0a0d
 jne .bad
 lea r12,[rbx+11]
.headers:
 cmp r12,r14
 jae .body
 mov rbx,r12
.colon:
 cmp rbx,r14
 jae .bad
 cmp byte [rbx],':'
 je .value
 cmp byte [rbx],33
 jb .bad
 inc rbx
 jmp .colon
.value:
 mov r15,rbx
 inc rbx
.skip:
 cmp rbx,r14
 jae .bad
 cmp byte [rbx],' '
 je .skipone
 cmp byte [rbx],9
 jne .vend
.skipone: inc rbx
 jmp .skip
.vend:
 mov rax,rbx
.vscan:
 cmp rax,r14
 jae .bad
 cmp word [rax],0x0a0d
 je .gotline
 inc rax
 jmp .vscan
.gotline:
 mov [rsp],rax ; ENTER reserves 8 scratch bytes
 mov rdx,r15
 sub rdx,r12
 mov rdi,r12
 lea rsi,[header_length]
 call ascii_equal
 test eax,eax
 jnz .length
 mov rdx,r15
 sub rdx,r12
 mov rdi,r12
 lea rsi,[header_transfer]
 call ascii_equal
 test eax,eax
 jnz .transfer
 mov rdx,r15
 sub rdx,r12
 mov rdi,r12
 lea rsi,[header_auth]
 call ascii_equal
 test eax,eax
 jnz .auth
 mov rdx,r15
 sub rdx,r12
 mov rdi,r12
 lea rsi,[header_expect]
 call ascii_equal
 test eax,eax
 jnz .expect
 mov rdx,r15
 sub rdx,r12
 mov rdi,r12
 lea rsi,[header_connection]
 call ascii_equal
 test eax,eax
 jnz .connection
 jmp .next
.length:
 cmp dword [length_seen],0
 jne .bad ; reject duplicate framing even if values agree
 mov dword [length_seen],1
 mov rdi,rbx
 mov rsi,[rsp]
 sub rsi,rbx
 cmp rsi,1
 jne .parselen
 cmp byte [rbx],'0'
 jne .parselen
 xor eax,eax
 jmp .lenok
.parselen:
 call parse_uint
 test rax,rax
 js .bad
.lenok:
 cmp rax,16384
 ja .large
 mov [content_length],rax
 jmp .next
.transfer:
 cmp dword [transfer_seen],0
 jne .bad
 mov dword [transfer_seen],1
 mov rdi,rbx
 lea rsi,[chunked_word]
 mov rdx,[rsp]
 sub rdx,rbx
 call ascii_equal
 test eax,eax
 jz .bad
 jmp .next
.auth:
 cmp qword [auth_ptr],0
 jne .bad
 mov [auth_ptr],rbx
 mov rax,[rsp]
 sub rax,rbx
 mov [auth_len],rax
 jmp .next
.expect:
 mov rdi,rbx
 lea rsi,[continue_word]
 mov rdx,[rsp]
 sub rdx,rbx
 call ascii_equal
 test eax,eax
 jz .bad
 mov dword [expect_seen],1
 jmp .next
.connection:
 mov rdi,rbx
 lea rsi,[close_word]
 mov rdx,[rsp]
 sub rdx,rbx
 call ascii_equal
 test eax,eax
 jz .next
 mov dword [request_close],1
.next:
 mov r12,[rsp]
 add r12,2
 jmp .headers
.body:
 mov r12,[rbp]
 add r12,[header_bytes]
 mov [body_ptr],r12
 cmp dword [transfer_seen],0
 je .fixedbody
 cmp dword [length_seen],0
 jne .bad
 ; Chunk decoder, maximum decoded body 16KiB, no live heap allocation.
 lea r15,[chunk_body]
 xor ebx,ebx
.chunk:
 xor r14d,r14d
 xor ecx,ecx
.hex:
 cmp r12,r13
 jae .incomplete
 movzx eax,byte [r12]
 cmp al,13
 je .chunksize
 cmp al,';'
 je .extension
 cmp al,'0'
 jb .bad
 cmp al,'9'
 jbe .digit
 or al,32
 cmp al,'a'
 jb .bad
 cmp al,'f'
 ja .bad
 sub eax,'a'-10
 jmp .hexadd
.digit: sub eax,'0'
.hexadd:
 shl r14,4
 add r14,rax
 cmp r14,16384
 ja .large
 inc ecx
 inc r12
 jmp .hex
.extension:
 inc r12
 cmp r12,r13
 jae .incomplete
 cmp byte [r12],13
 jne .extension
.chunksize:
 test ecx,ecx
 jz .bad
 lea rax,[r12+2]
 cmp rax,r13
 ja .incomplete
 cmp word [r12],0x0a0d
 jne .bad
 add r12,2
 test r14,r14
 jz .trailers
 mov rax,r14
 add rax,rbx
 cmp rax,16384
 ja .large
 lea rax,[r12+r14+2]
 cmp rax,r13
 ja .incomplete
 cmp word [r12+r14],0x0a0d
 jne .bad
 mov rdi,r15
 mov rsi,r12
 mov rcx,r14
 rep movsb
 mov r15,rdi
 add rbx,r14
 lea r12,[r12+r14+2]
 jmp .chunk
.trailers:
 ; Validate bounded trailer lines; no trailer alters request framing.
 lea rax,[r12+2]
 cmp rax,r13
 ja .incomplete
 cmp word [r12],0x0a0d
 je .chunkdone
.trailerline:
 lea rax,[r12+2]
 cmp rax,r13
 ja .incomplete
 cmp word [r12],0x0a0d
 je .trailerend
 inc r12
 jmp .trailerline
.trailerend:
 add r12,2
 jmp .trailers
.chunkdone:
 add r12,2
 mov rax,r12
 sub rax,[rbp]
 mov [request_bytes],rax
 lea rax,[chunk_body]
 mov [body_ptr],rax
 mov [body_len],rbx
 mov eax,1
 jmp .exit
.fixedbody:
 mov rax,[content_length]
 mov [body_len],rax
 add rax,[header_bytes]
 cmp rax,[rbp+8]
 ja .incomplete
 mov [request_bytes],rax
 mov eax,1
 jmp .exit
.incomplete_header:
 mov rax,r13
 sub rax,[rbp]
 cmp rax,16384
 jae .large
.incomplete:
 xor eax,eax
 jmp .exit
.bad: mov eax,-1
 jmp .exit
.large: mov eax,-2
.exit: LEAVE
; Output helpers write bounded response scratch; bodies have a prevalidated <OUT_CAP bound.
append_z:
.loop:
 mov al,[rsi]
 test al,al
 jz .done
 mov [rdi],al
 inc rdi
 inc rsi
 jmp .loop
.done: ret
append_uint:
 sub rsp,40
 lea rsi,[rsp+32]
 mov ecx,10
.loop:
 xor edx,edx
 div rcx
 add dl,'0'
 dec rsi
 mov [rsi],dl
 test rax,rax
 jnz .loop
 lea rcx,[rsp+32]
 sub rcx,rsi
 rep movsb
 add rsp,40
 ret
; Authenticate into user_id / stable user_name. -> 0 valid, 1 missing, 2 token, 3 payload.
authenticate:
 ENTER
 mov r12,[auth_ptr]
 mov r13,[auth_len]
 cmp r13,7
 jb .missing
 mov rdi,r12
 lea rsi,[bearer]
 mov edx,7
 call bytes_equal
 test eax,eax
 jz .missing
 add r12,7
 sub r13,7
 cmp r13,8192
 ja .invalid
 mov rbx,r12
 lea rbp,[r12+r13]
.find1:
 cmp rbx,rbp
 jae .invalid
 cmp byte [rbx],'.'
 je .first
 inc rbx
 jmp .find1
.first:
 lea r14,[rbx+1]
 mov r15,r14
.find2:
 cmp r15,rbp
 jae .invalid
 cmp byte [r15],'.'
 je .second
 inc r15
 jmp .find2
.second:
 mov rdi,r12
 mov rsi,rbx
 sub rsi,r12
 lea rdx,[jwt_header]
 mov ecx,8192
 call b64decode
 test rax,rax
 js .invalid
 mov [rsp],rax
 mov rdi,r14
 mov rsi,r15
 sub rsi,r14
 lea rdx,[jwt_payload]
 mov ecx,8192
 call b64decode
 test rax,rax
 js .invalid
 mov r13,rax
 lea rdi,[r15+1]
 mov rsi,rbp
 sub rsi,rdi
 lea rdx,[jwt_sig]
 mov ecx,64
 call b64decode
 cmp rax,32
 jne .invalid
 mov rdi,r12
 mov rsi,r15
 sub rsi,r12
 lea rdx,[jwt_expected]
 call hmac_sha256
 ; Constant-time full-length signature comparison.
 lea rsi,[jwt_sig]
 lea rdi,[jwt_expected]
 xor eax,eax
 xor ecx,ecx
.sig:
 mov dl,[rsi+rcx]
 xor dl,[rdi+rcx]
 or al,dl
 inc ecx
 cmp ecx,32
 jb .sig
 test eax,eax
 jnz .invalid
 lea rdi,[jwt_header]
 mov rsi,[rsp]
 call json_parse
 cmp eax,3
 jne .invalid
 cmp qword [json_fields],1
 jne .invalid
 cmp qword [json_fields+16],5
 jne .invalid
 mov rdi,[json_fields+8]
 lea rsi,[hs256]
 mov edx,5
 call bytes_equal
 test eax,eax
 jz .invalid
 lea rdi,[jwt_payload]
 mov rsi,r13
 call json_parse
 test eax,eax
 js .invalid
 cmp eax,3
 jne .payload
 cmp qword [json_fields+72],2 ; exp number
 jne .invalid
 mov rdi,[json_fields+80]
 mov rsi,[json_fields+88]
 call numeric_date
 test eax,eax
 js .invalid
 movq r14,xmm0
 mov eax,201 ; expiration uses wall time, timeout wheel uses monotonic time
 xor edi,edi
 syscall
 cvtsi2sd xmm1,rax
 movq xmm0,r14
 comisd xmm1,xmm0
 jae .invalid
 cmp qword [json_fields+96],0
 je .claims
 cmp qword [json_fields+96],2
 jne .invalid
 mov rdi,[json_fields+104]
 mov rsi,[json_fields+112]
 call numeric_date
 test eax,eax
 js .invalid
 movq r14,xmm0
 mov eax,201
 xor edi,edi
 syscall
 cvtsi2sd xmm1,rax
 movq xmm0,r14
 comisd xmm1,xmm0
 jb .invalid
.claims:
 cmp qword [json_fields+24],1
 jne .payload
 mov rdi,[json_fields+32]
 mov rsi,[json_fields+40]
 call parse_uint
 test rax,rax
 js .payload
 mov [user_id],rax
 cmp qword [json_fields+48],1
 jne .payload
 mov rcx,[json_fields+64]
 cmp rcx,8192
 ja .payload
 mov [user_name_len],rcx
 mov rsi,[json_fields+56]
 lea rdi,[user_name]
 rep movsb
 xor eax,eax
 jmp .exit
.missing: mov eax,1
 jmp .exit
.invalid: mov eax,2
 jmp .exit
.payload: mov eax,3
.exit: LEAVE
; Execute prepared transaction/control statement by index. Never prepare on request path.
control_step:
 ENTER
 lea rax,[statements]
 mov r12,[rax+rdi*8]
 mov rdi,r12
 call sqlite3_step
 mov ebx,eax
 mov rdi,r12
 call sqlite3_reset
 cmp ebx,101
 jne .bad
 test eax,eax
 jnz .bad
 xor eax,eax
 jmp .exit
.bad: mov eax,-1
.exit: LEAVE
; dispatch -> response_body/response_len/response_status. All writes commit before return.
dispatch:
 ENTER
 cmp dword [batch_failed],0
 jne .internal
 cmp dword [group_context],0
 je .start
 cmp dword [transaction_mode],2
 jne .start
 call batch_begin
 test eax,eax
 jnz .internal
.start:
 mov dword [response_status],500
 lea rdi,[response_body]
 mov qword [response_len],0
 cmp dword [method],1
 jne .post_method
 mov rdi,[path_ptr]
 lea rsi,[path_health]
 mov rdx,[path_len]
 call .path_equal
 test eax,eax
 jnz .health
 mov rdi,[path_ptr]
 lea rsi,[path_feed]
 mov rdx,[path_len]
 call .path_equal
 test eax,eax
 jnz .feed
 mov dword [route],1
 jmp .post_path
.post_method:
 cmp dword [method],2
 jne .notfound
 mov rdi,[path_ptr]
 lea rsi,[path_posts]
 mov rdx,[path_len]
 call .path_equal
 test eax,eax
 jnz .create_auth
 mov dword [route],2
 ; Like paths require /posts/ prefix and /like suffix before authentication.
 mov rax,[path_len]
 cmp rax,12
 jb .notfound
 mov rdi,[path_ptr]
 add rdi,rax
 sub rdi,5
 lea rsi,[suffix_like]
 mov edx,5
 call bytes_equal
 test eax,eax
 jz .notfound
.post_path:
 cmp qword [path_len],7
 jb .notfound
 mov rdi,[path_ptr]
 lea rsi,[prefix_posts]
 mov edx,7
 call bytes_equal
 test eax,eax
 jz .notfound
 cmp dword [route],2
 jne .parse_id
 call authenticate
 test eax,eax
 jnz .auth_error
.parse_id:
 mov rdi,[path_ptr]
 add rdi,7
 mov rsi,[path_len]
 sub rsi,7
 cmp dword [route],2
 jne .id
 sub rsi,5
.id:
 call parse_uint
 test rax,rax
 js .invalid_id
 mov [post_id],rax
 cmp dword [route],2
 je .like
 mov r12,[statements+16]
 mov rdi,r12
 mov esi,1
 mov rdx,rax
 call sqlite3_bind_int64
 test eax,eax
 jnz .readfail
 jmp .readjson
.health:
 mov r12,[statements]
 mov rdi,r12
 call sqlite3_step
 cmp eax,100
 jne .healthfail
 mov rdi,r12
 call sqlite3_step
 mov ebx,eax
 mov rdi,r12
 call sqlite3_reset
 cmp ebx,101
 jne .healthfail
 test eax,eax
 jnz .healthfail
 lea rdi,[response_body]
 APPEND health_prefix
 mov rax,[now_sec]
 sub rax,[boot_sec]
 call append_uint
 mov byte [rdi],'}'
 inc rdi
 mov dword [response_status],200
 jmp .built
.healthfail:
 mov rdi,r12
 call sqlite3_reset
 mov dword [response_status],503
 lea rsi,[err_health]
 jmp .error
.feed:
 cmp dword [query_mode],3
 jne .feed_reference
 call ranges_feed
 test eax,eax
 jnz .internal
 mov dword [response_status],200
 jmp .exit
.feed_reference:
 mov r12,[statements+8]
.readjson:
 cmp dword [json_mode],1
 je .read_nasm
 cmp dword [json_mode],2
 jne .sql_json
 cmp r12,[statements+16]
 je .read_nasm
.sql_json:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .missing_read
 cmp eax,100
 jne .readfail
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_text
 test rax,rax
 jz .readfail
 mov r13,rax
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_bytes
 cmp eax,65000
 ja .readfail
 mov ebx,eax
 mov rsi,r13
 lea rdi,[response_body]
 mov ecx,ebx
 rep movsb
 mov [response_len],rbx
 mov rdi,r12
 call sqlite3_step
 mov ebx,eax
 mov rdi,r12
 call sqlite3_reset
 cmp ebx,101
 jne .internal
 test eax,eax
 jnz .internal
 mov dword [response_status],200
 jmp .exit
.read_nasm:
 lea rdi,[response_body]
 APPEND single_prefix
 cmp r12,[statements+16]
 je .nasm_begin
 lea rdi,[response_body]
 APPEND feed_prefix
.nasm_begin:
 mov r13,rdi
 xor r14d,r14d
.nasm_row:
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 je .nasm_done
 cmp eax,100
 jne .readfail
 mov rdi,r13
 test r14d,r14d
 jz .nasm_first
 mov byte [rdi],','
 inc rdi
.nasm_first:
 mov rsi,r12
 call append_post
 test rax,rax
 js .readfail
 mov r13,rax
 inc r14d
 jmp .nasm_row
.nasm_done:
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .internal
 mov rdi,r13
 cmp r12,[statements+16]
 jne .nasm_feed_end
 test r14d,r14d
 jz .missing_post
 jmp .nasm_end
.nasm_feed_end:
 mov byte [rdi],']'
 inc rdi
.nasm_end:
 mov byte [rdi],'}'
 inc rdi
 mov dword [response_status],200
 jmp .built
.missing_read:
 mov rdi,r12
 call sqlite3_reset
 jmp .missing_post
.readfail:
 mov rdi,r12
 call sqlite3_reset
 jmp .internal
.create_auth:
 call authenticate
 test eax,eax
 jnz .auth_error
 mov rdi,[body_ptr]
 mov rsi,[body_len]
 call json_parse
 test eax,eax
 js .malformed
 cmp qword [json_fields+120],1
 jne .required
 mov rdi,[json_fields+128]
 mov rsi,[json_fields+136]
 call trim_utf8
 test rax,rax
 js .malformed
 test rdx,rdx
 jz .required
 cmp ecx,500
 ja .toolong
 mov [post_text_len],rdx
 mov rsi,rax
 lea rdi,[post_text]
 mov rcx,rdx
 rep movsb
 call write_begin
 test eax,eax
 jnz .internal
 mov r12,[statements+24]
 mov rdi,r12
 mov esi,1
 mov rdx,[user_id]
 call sqlite3_bind_int64
 test eax,eax
 jnz .writefail
 mov rdi,r12
 mov esi,2
 lea rdx,[post_text]
 mov rcx,[post_text_len]
 xor r8d,r8d ; SQLITE_STATIC, survives through reset
 call sqlite3_bind_text
 test eax,eax
 jnz .writefail
 mov rdi,r12
 call sqlite3_step
 cmp eax,100
 jne .writefail
 mov rdi,r12
 xor esi,esi
 call sqlite3_column_int64
 mov [post_id],rax
 mov rdi,r12
 mov esi,1
 call sqlite3_column_text
 mov r13,rax
 mov rdi,r12
 mov esi,1
 call sqlite3_column_bytes
 cmp eax,24
 jne .writefail
 mov rsi,r13
 lea rdi,[new_created]
 mov ecx,24
 rep movsb
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 jne .writefail
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .rollback
 call write_commit
 test eax,eax
 jnz .rollback
 ; Response construction occurs after COMMIT, no tentative success can escape.
 lea rdi,[response_body]
 APPEND post_prefix
 mov rax,[post_id]
 call append_uint
 APPEND body_prefix
 lea rsi,[post_text]
 mov rdx,[post_text_len]
 call json_quote
 mov rdi,rax
 APPEND created_prefix
 lea rsi,[new_created]
 mov edx,24
 call json_quote
 mov rdi,rax
 APPEND author_prefix
 lea rsi,[user_name]
 mov rdx,[user_name_len]
 call json_quote
 mov rdi,rax
 APPEND post_end
 mov dword [response_status],201
 jmp .built
.like:
 call write_begin
 test eax,eax
 jnz .internal
 mov r12,[statements+32]
 mov rdi,r12
 mov esi,1
 mov rdx,[user_id]
 call sqlite3_bind_int64
 test eax,eax
 jnz .writefail
 mov rdi,r12
 mov esi,2
 mov rdx,[post_id]
 call sqlite3_bind_int64
 test eax,eax
 jnz .writefail
 mov rdi,r12
 call sqlite3_step
 cmp eax,101
 jne .writefail
 mov rdi,[database]
 call sqlite3_changes
 mov r13d,eax
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .rollback
 test r13d,r13d
 jnz .likecommit
 mov r12,[statements+40]
 mov rdi,r12
 mov esi,1
 mov rdx,[post_id]
 call sqlite3_bind_int64
 test eax,eax
 jnz .writefail
 mov rdi,r12
 call sqlite3_step
 mov r14d,eax
 mov rdi,r12
 call sqlite3_reset
 test eax,eax
 jnz .rollback
 cmp r14d,100
 je .likecommit
 cmp r14d,101
 jne .rollback
 call write_commit
 test eax,eax
 jnz .rollback
 jmp .missing_post
.likecommit:
 call write_commit
 test eax,eax
 jnz .rollback
 lea rdi,[response_body]
 mov dword [response_status],200
 lea rsi,[like_repeat]
 test r13d,r13d
 jz .likejson
 mov dword [response_status],201
 lea rsi,[like_first]
.likejson:
 call append_z
 mov rax,[post_id]
 call append_uint
 mov byte [rdi],'}'
 inc rdi
 jmp .built
.writefail:
 mov [write_error],eax
 mov rdi,r12
 call sqlite3_reset
.rollback:
 call write_rollback
 cmp dword [batch_active],0
 jne .internal
 mov rdi,[database]
 call sqlite3_get_autocommit
 test eax,eax
 jz fatal ; cannot continue on an unknown transactional state
.internal:
 mov dword [response_status],500
 lea rsi,[err_internal]
 jmp .error
.auth_error:
 mov dword [response_status],401
 lea rsi,[err_bearer]
 cmp eax,1
 je .error
 lea rsi,[err_token]
 cmp eax,2
 je .error
 lea rsi,[err_payload]
 jmp .error
.notfound:
 mov dword [response_status],404
 lea rsi,[err_notfound]
 jmp .error
.invalid_id:
 mov dword [response_status],400
 lea rsi,[err_id]
 jmp .error
.missing_post:
 mov dword [response_status],404
 lea rsi,[err_post]
 jmp .error
.malformed:
 mov dword [response_status],400
 lea rsi,[err_json]
 jmp .error
.required:
 mov dword [response_status],400
 lea rsi,[err_body]
 jmp .error
.toolong:
 mov dword [response_status],400
 lea rsi,[err_long]
.error:
 lea rdi,[response_body]
 call append_z
.built:
 lea rax,[response_body]
 sub rdi,rax
 mov [response_len],rdi
.exit:
 inc qword [request_count]
 cmp dword [batch_active],0
 jne .return
 call maybe_checkpoint
.return: LEAVE
.path_equal:
 ; exact comparison with zero-terminated constant, preserving request registers
 xor eax,eax
 xor ecx,ecx
.pe:
 cmp rcx,rdx
 jae .pend
 mov r8b,[rdi+rcx]
 cmp r8b,[rsi+rcx]
 jne .pret
 inc rcx
 jmp .pe
.pend:
 cmp byte [rsi+rcx],0
 sete al
.pret: ret
; Build HTTP header/body into already-reserved output memory.
build_response:
 ENTER
 mov r12,rdi
 mov rdi,[r12+16]
 lea rsi,[status_200]
 mov eax,[response_status]
 cmp eax,201
 jne .s400
 lea rsi,[status_201]
 jmp .header
.s400:
 cmp eax,400
 jne .s401
 lea rsi,[status_400]
 jmp .header
.s401:
 cmp eax,401
 jne .s404
 lea rsi,[status_401]
 jmp .header
.s404:
 cmp eax,404
 jne .s413
 lea rsi,[status_404]
 jmp .header
.s413:
 cmp eax,413
 jne .s500
 lea rsi,[status_413]
 jmp .header
.s500:
 cmp eax,500
 jne .s503
 lea rsi,[status_500]
 jmp .header
.s503:
 cmp eax,503
 jne .header
 lea rsi,[status_503]
.header:
 call append_z
 APPEND content_header
 mov rax,[response_len]
 call append_uint
 lea rsi,[keep_header]
 test dword [r12+60],2
 jz .endheaders
 lea rsi,[close_header]
.endheaders:
 call append_z
 lea rsi,[response_body]
 mov rcx,[response_len]
 rep movsb
 sub rdi,[r12+16]
 mov [r12+24],rdi
 mov qword [r12+32],0
 LEAVE
; fd edi, epoll flags esi. Single in-flight response per socket; pipeline input is retained.
connection_event:
 ENTER
 mov ebx,edi
 cmp ebx,MAX_FD
 jae .done
 mov rax,rdi
 shr rax,32
 lea rdx,[generations]
 cmp eax,[rdx+rbx*4]
 jne .done
 mov r12d,ebx
 shl r12,6
 lea rax,[connections]
 add r12,rax
 test dword [r12+60],1
 jz .done
 test esi,0x18 ; EPOLLERR/HUP (RDHUP can accompany readable final request)
 jnz .close
 cmp qword [r12+24],0
 jne .send
.read:
 cmp qword [r12],0
 jne .recv
 ; Reserve a full bounded upload buffer on admission. Admitted uploads never need
 ; additional input allocation, so aggregate exhaustion cannot deadlock completion.
 cmp qword [input_allocated],28*1024*1024
 jae .pressure
 cmp qword [output_allocated],64*1024*1024
 jae .pressure
 call allocate_input
 test rax,rax
 js .close
 mov [r12],rax
 add qword [input_allocated],IN_CAP
 call .reserve_output
 test rax,rax
 js .close
.recv:
 mov eax,45
 mov edi,ebx
 mov rsi,[r12]
 add rsi,[r12+8]
 mov edx,IN_CAP
 sub rdx,[r12+8]
 jz .bad
 xor r10d,r10d
 xor r8d,r8d
 xor r9d,r9d
 syscall
 test rax,rax
 jz .close
 cmp eax,-4
 je .recv
 cmp eax,-11
 je .done
 test rax,rax
 js .close
 add [r12+8],rax
 mov edi,ebx
 call timer_touch
.parse:
 cmp qword [r12+8],0
 je .release_input
 call .reserve_output
 test rax,rax
 js .close
 mov rdi,r12
 call parse_http
 test eax,eax
 jz .incomplete
 js .parse_error
 ; Reserve response capacity BEFORE touching SQLite.
 cmp qword [output_allocated],64*1024*1024
 jae .pressure
 call .reserve_output
 test rax,rax
 js .close
 cmp dword [request_close],0
 je .dispatch
 or dword [r12+60],2
.dispatch:
 call dispatch
 mov rdi,r12
 call build_response
 ; consume this request, preserving pipelined bytes (forward copy: dst <= src)
 mov rcx,[r12+8]
 sub rcx,[request_bytes]
 mov [r12+8],rcx
 mov rsi,[r12]
 add rsi,[request_bytes]
 mov rdi,[r12]
 rep movsb
 and dword [r12+60],~4
 jmp .send
.incomplete:
 cmp dword [expect_seen],0
 je .recv
 test dword [r12+60],4
 jnz .recv
 call .reserve_output
 test rax,rax
 js .close
 mov rdi,[r12+16]
 lea rsi,[interim]
 mov ecx,interim_len
 rep movsb
 mov qword [r12+24],interim_len
 mov qword [r12+32],0
 or dword [r12+60],12 ; continue sent, interim response
 jmp .send
.parse_error:
 mov dword [response_status],400
 cmp eax,-2
 jne .errorbody
 mov dword [response_status],413
.errorbody:
 lea rsi,[err_json]
 lea rdi,[response_body]
 call append_z
 lea rax,[response_body]
 sub rdi,rax
 mov [response_len],rdi
 or dword [r12+60],2
 call .reserve_output
 test rax,rax
 js .close
 mov rdi,r12
 call build_response
 jmp .send
.bad:
 or dword [r12+60],2
 jmp .close
.pressure:
 ; Do not allocate more input. Drain any already reserved response first.
 cmp qword [r12+16],0
 jne .send
 jmp .close
.send:
 mov eax,44
 mov edi,ebx
 mov rsi,[r12+16]
 add rsi,[r12+32]
 mov rdx,[r12+24]
 sub rdx,[r12+32]
 mov r10d,0x4000 ; MSG_NOSIGNAL
 xor r8d,r8d
 xor r9d,r9d
 syscall
 cmp eax,-4
 je .send
 cmp eax,-11
 je .want_output
 test rax,rax
 jle .close
 add [r12+32],rax
 mov edi,ebx
 call timer_touch
 mov rax,[r12+32]
 cmp rax,[r12+24]
 jb .send
 mov rdi,[r12+16]
 call release_output
 mov qword [r12+16],0
 mov qword [r12+24],0
 mov qword [r12+32],0
 sub qword [output_allocated],OUT_CAP
 test dword [r12+60],2
 jnz .close
 and dword [r12+60],~8
 mov edi,ebx
 mov esi,0x2001
 call update_interest
 cmp qword [r12+8],0
 jne .parse
.release_input:
 cmp qword [r12],0
 je .idle
 mov rdi,[r12]
 call release_input
 mov qword [r12],0
 sub qword [input_allocated],IN_CAP
.idle:
 mov edi,ebx
 call timer_touch
 jmp .done
.want_output:
 mov edi,ebx
 mov esi,0x2004
 call update_interest
 jmp .done
.close:
 mov edi,ebx
 call close_connection
.done: LEAVE
.reserve_output:
 sub rsp,8
 cmp qword [r12+16],0
 jne .reserved
 cmp qword [output_allocated],64*1024*1024
 jae .nomem
 call allocate_output
 test rax,rax
 js .reserve_done
 mov [r12+16],rax
 add qword [output_allocated],OUT_CAP
.reserved:
 mov rax,[r12+16]
.reserve_done:
 add rsp,8
 ret
.nomem:
 mov rax,-1
 jmp .reserve_done
update_interest:
 mov [ctl_event],esi
 lea rax,[generations]
 mov eax,[rax+rdi*4]
 shl rax,32
 or rax,rdi
 mov [ctl_event+4],rax
 mov edx,edi
 mov eax,233
 mov edi,[epoll_fd]
 mov esi,3
 lea r10,[ctl_event]
 syscall
 ret
; Bounded owner-thread slabs: no mmap/munmap in warmed request processing.
allocate_input:
 mov rax,[input_free]
 test rax,rax
 jz .bump
 mov rdx,[rax]
 mov [input_free],rdx
 ret
.bump:
 mov rdx,[input_bump]
 cmp rdx,32*1024*1024
 jae allocation_failed
 lea rax,[input_pool]
 add rax,rdx
 add qword [input_bump],IN_CAP
 ret
release_input:
 mov rax,[input_free]
 mov [rdi],rax
 mov [input_free],rdi
 ret
allocate_output:
 mov rax,[output_free]
 test rax,rax
 jz .bump
 mov rdx,[rax]
 mov [output_free],rdx
 ret
.bump:
 mov rdx,[output_bump]
 cmp rdx,64*1024*1024
 jae allocation_failed
 lea rax,[output_pool]
 add rax,rdx
 add qword [output_bump],OUT_CAP
 ret
release_output:
 mov rax,[output_free]
 mov [rdi],rax
 mov [output_free],rdi
 ret
allocation_failed:
 mov rax,-12
 ret
wal_hook:
 mov [wal_frames],ecx
 xor eax,eax
 ret
maybe_checkpoint:
 ENTER
 cmp dword [wal_frames],1000
 jb .done
 mov rdi,[database]
 xor esi,esi
 xor edx,edx
 lea rcx,[wal_log]
 lea r8,[wal_checkpointed]
 call sqlite3_wal_checkpoint_v2
 test eax,eax
 jnz .done
 mov eax,[wal_log]
 cmp eax,[wal_checkpointed]
 jne .done
 mov dword [wal_frames],0
.done: LEAVE
%include "experiments.inc"
%include "uring.inc"
section .note.GNU-stack noalloc noexec nowrite progbits

%include "ranges.inc"
