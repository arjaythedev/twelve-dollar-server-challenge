; Feed API server: raw Linux syscalls for sockets/HTTP, SQLite through its C
; API, one worker thread per accepted keep-alive connection (Nginx mode).

%include "common.inc"

extern malloc
extern getenv
extern strlen
extern atoi
extern inet_pton
extern signal
extern pthread_create
extern pthread_join

extern db_init
extern db_close
extern db_health
extern db_feed
extern db_get_post
extern db_create_post
extern db_like
extern g_sqlite_path

extern b64url_init
extern jwt_auth

extern find_header_end
extern find_byte
extern mem_eq
extern mem_copy
extern starts_with
extern trim_ws
extern u64_parse
extern append_bytes
extern append_u64
extern json_parse_body
extern ci_eq

%define N_WORKERS 64

section .data
one:            dd 1

section .rodata
env_sqlite:     db 'SQLITE_PATH', 0
env_jwt:        db 'JWT_SECRET', 0
env_host:       db 'HOST', 0
env_port:       db 'PORT', 0
default_host:   db '127.0.0.1', 0
default_port:   db '3000', 0

s_get:          db 'GET'
s_post:         db 'POST'
s_http11:       db 'HTTP/1.1'
s_http10:       db 'HTTP/1.0'
s_close:        db 'close'
s_keep:         db 'keep-alive'
s_content_len:  db 'content-length'
s_auth:         db 'authorization'
s_connection:   db 'connection'

p_health:       db '/health'
p_feed:         db '/feed'
p_posts:        db '/posts'
p_posts_slash:  db '/posts/'
p_like:         db '/like'

s_200: db 'HTTP/1.1 200 OK'
s_200_len equ $ - s_200
s_201: db 'HTTP/1.1 201 Created'
s_201_len equ $ - s_201
s_400: db 'HTTP/1.1 400 Bad Request'
s_400_len equ $ - s_400
s_401: db 'HTTP/1.1 401 Unauthorized'
s_401_len equ $ - s_401
s_404: db 'HTTP/1.1 404 Not Found'
s_404_len equ $ - s_404
s_500: db 'HTTP/1.1 500 Internal Server Error'
s_500_len equ $ - s_500
s_503: db 'HTTP/1.1 503 Service Unavailable'
s_503_len equ $ - s_503

status_ptrs:
    dq s_200, s_201, s_400, s_401, s_404, s_500, s_503
status_lens:
    dd s_200_len, s_201_len, s_400_len, s_401_len, s_404_len, s_500_len, s_503_len

s_hdr_type: db 13,10,'Content-Type: application/json',13,10,'Content-Length: '
s_hdr_type_len equ $ - s_hdr_type
s_hdr_conn: db 13,10,'Connection: '
s_hdr_conn_len equ $ - s_hdr_conn
s_hdr_end:  db 13,10,13,10
s_hdr_end_len equ $ - s_hdr_end

e_notfound:       db '{"error":"not found"}'
e_notfound_len equ $ - e_notfound
e_badid:          db '{"error":"invalid post id"}'
e_badid_len equ $ - e_badid
e_postnotfound:   db '{"error":"post not found"}'
e_postnotfound_len equ $ - e_postnotfound
e_missingbearer:  db '{"error":"missing bearer token"}'
e_missingbearer_len equ $ - e_missingbearer
e_invalidtoken:   db '{"error":"invalid or expired token"}'
e_invalidtoken_len equ $ - e_invalidtoken
e_invalidpayload: db '{"error":"invalid token payload"}'
e_invalidpayload_len equ $ - e_invalidpayload
e_malformedjson:  db '{"error":"malformed JSON body"}'
e_malformedjson_len equ $ - e_malformedjson
e_bodyrequired:   db '{"error":"body is required"}'
e_bodyrequired_len equ $ - e_bodyrequired
e_bodylong:       db '{"error":"body must be at most 500 characters"}'
e_bodylong_len equ $ - e_bodylong
e_internal:       db '{"error":"internal server error"}'
e_internal_len equ $ - e_internal
e_badrequest:     db '{"error":"bad request"}'
e_badrequest_len equ $ - e_badrequest

b_health_open:  db '{"status":"ok","db":"ok","uptime_s":'
b_health_open_len equ $ - b_health_open
b_health_bad:   db '{"status":"degraded","db":"unreachable","error":"database error"}'
b_health_bad_len equ $ - b_health_bad

b_like_new: db '{"liked":true,"already_liked":false,"post_id":'
b_like_new_len equ $ - b_like_new
b_like_old: db '{"liked":true,"already_liked":true,"post_id":'
b_like_old_len equ $ - b_like_old

msg_no_db: db 'feed: SQLITE_PATH is required',10
msg_no_db_len equ $ - msg_no_db
msg_listen: db 'feed: cannot listen',10
msg_listen_len equ $ - msg_listen

section .bss
g_listen_fd:     resd 1
g_jwt_secret:    resq 1
g_jwt_secret_len: resq 1
g_start_sec:     resq 1
g_start_nsec:    resq 1
g_timeout:       resq 2
sockaddr_in:     resb 16
worker_ctxs:     resb W_SIZE * N_WORKERS
thread_ids:      resq N_WORKERS

section .text

global main
global worker_entry

; -----------------------------------------------------------------------------
; die(rdi=msg, rsi=len) - write to stderr and exit(1)
; -----------------------------------------------------------------------------
die:
    mov rdx, rsi
    mov rsi, rdi
    mov edi, 2
    mov eax, SYS_write
    syscall
    mov eax, 60
    mov edi, 1
    syscall

; -----------------------------------------------------------------------------
; main
; -----------------------------------------------------------------------------
main:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    ; SIGPIPE -> ignore
    mov edi, 13
    mov esi, 1
    call signal
    call b64url_init

    lea rdi, [rel env_sqlite]
    call getenv
    test rax, rax
    jz .no_db
    mov [rel g_sqlite_path], rax

    lea rdi, [rel env_jwt]
    call getenv
    test rax, rax
    jz .secret_done
    mov [rel g_jwt_secret], rax
    mov rdi, rax
    call strlen
    mov [rel g_jwt_secret_len], rax
.secret_done:

    lea rdi, [rel env_host]
    call getenv
    test rax, rax
    jnz .have_host
    lea rax, [rel default_host]
.have_host:
    mov r13, rax
    lea rdi, [rel env_port]
    call getenv
    test rax, rax
    jnz .have_port
    lea rax, [rel default_port]
.have_port:
    mov rdi, rax
    call atoi
    mov ebx, eax

    ; monotonic start time
    mov eax, SYS_clock_gettime
    mov edi, CLOCK_MONOTONIC
    lea rsi, [rbp-64]
    syscall
    mov rax, [rbp-64]
    mov [rel g_start_sec], rax
    mov rax, [rbp-56]
    mov [rel g_start_nsec], rax

    ; listening socket
    mov eax, SYS_socket
    mov edi, AF_INET
    mov esi, SOCK_STREAM
    xor edx, edx
    syscall
    test rax, rax
    js .bad_listen
    mov r12d, eax
    mov [rel g_listen_fd], eax

    mov eax, SYS_setsockopt
    mov edi, r12d
    mov esi, SOL_SOCKET
    mov edx, SO_REUSEADDR
    lea r10, [rel one]
    mov r8d, 4
    syscall

    ; sockaddr_in
    lea r14, [rel sockaddr_in]
    mov word [r14], AF_INET
    mov eax, ebx
    rol ax, 8
    mov [r14+2], ax
    mov dword [r14+4], 0
    mov dword [r14+8], 0
    mov dword [r14+12], 0
    mov edi, AF_INET
    mov rsi, r13
    lea rdx, [r14+4]
    call inet_pton
    cmp eax, 1
    jne .bad_listen

    mov eax, SYS_bind
    mov edi, r12d
    mov rsi, r14
    mov edx, 16
    syscall
    test rax, rax
    js .bad_listen
    mov eax, SYS_listen
    mov edi, r12d
    mov esi, 512
    syscall
    test rax, rax
    js .bad_listen

    xor r15d, r15d
.spawn:
    cmp r15d, N_WORKERS
    jae .join_all
    mov rax, W_SIZE
    imul rax, r15
    lea rcx, [rel worker_ctxs]
    add rcx, rax
    lea rdi, [rel thread_ids]
    lea rdi, [rdi+r15*8]
    xor esi, esi
    lea rdx, [rel worker_entry]
    call pthread_create
    inc r15d
    jmp .spawn
.join_all:
    xor r15d, r15d
.join_loop:
    cmp r15d, N_WORKERS
    jae .hang
    lea rax, [rel thread_ids]
    mov rdi, [rax+r15*8]
    xor esi, esi
    call pthread_join
    inc r15d
    jmp .join_loop
.hang:
    mov qword [rel g_timeout], 1000000
    mov qword [rel g_timeout+8], 0
    mov eax, 35                 ; nanosleep
    lea rdi, [rel g_timeout]
    xor esi, esi
    syscall
    jmp .hang

.bad_listen:
    lea rdi, [rel msg_listen]
    mov esi, msg_listen_len
    call die
.no_db:
    lea rdi, [rel msg_no_db]
    mov esi, msg_no_db_len
    call die

; -----------------------------------------------------------------------------
; worker_entry(rdi=wctx) -> rax=0
; -----------------------------------------------------------------------------
worker_entry:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov rdi, r12
    call db_init
    test eax, eax
    jnz .exit
    mov edi, REQ_SIZE
    call malloc
    test rax, rax
    jz .exit_db
    mov [r12+W_REQ], rax
    mov edi, RESP_SIZE
    call malloc
    test rax, rax
    jz .exit_db
    mov [r12+W_RESP], rax
    mov edi, TMP_SIZE
    call malloc
    test rax, rax
    jz .exit_db
    mov [r12+W_TMP], rax
    mov edi, USER_SIZE
    call malloc
    test rax, rax
    jz .exit_db
    mov [r12+W_USER], rax
.loop:
    mov eax, SYS_accept
    mov edi, [rel g_listen_fd]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    js .accept_err
    mov r13d, eax
    mov qword [rel g_timeout], 75
    mov qword [rel g_timeout+8], 0
    mov eax, SYS_setsockopt
    mov edi, r13d
    mov esi, SOL_SOCKET
    mov edx, SO_RCVTIMEO
    lea r10, [rel g_timeout]
    mov r8d, 16
    syscall
    mov eax, SYS_setsockopt
    mov edi, r13d
    mov esi, IPPROTO_TCP
    mov edx, TCP_NODELAY
    lea r10, [rel one]
    mov r8d, 4
    syscall
    mov rdi, r13
    mov rsi, r12
    call handle_connection
    mov eax, SYS_close
    mov edi, r13d
    syscall
    jmp .loop
.accept_err:
    cmp rax, EINTR
    je .loop
    mov qword [rel g_timeout], 0
    mov qword [rel g_timeout+8], 1000000
    mov eax, 35
    lea rdi, [rel g_timeout]
    xor esi, esi
    syscall
    jmp .loop
.exit_db:
    mov rdi, r12
    call db_close
.exit:
    xor eax, eax
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; handle_connection(rdi=fd, rsi=wctx)
; -----------------------------------------------------------------------------
handle_connection:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    mov r12, rdi
    mov r13, rsi
    mov r14, [r13+W_REQ]
    xor r15d, r15d
    mov rax, [r13+W_RESP]
    mov [rbp-176], rax
.read_headers:
    mov rdi, r14
    mov rsi, r15
    call find_header_end
    cmp rax, -1
    jne .have_headers
    cmp r15, REQ_SIZE
    jae .bad_request
    mov rdx, REQ_SIZE
    sub rdx, r15
    mov eax, SYS_read
    mov edi, r12d
    lea rsi, [r14+r15]
    syscall
    test rax, rax
    jle .read_error
    add r15, rax
    jmp .read_headers
.read_error:
    cmp rax, EINTR
    je .read_headers
    jmp .close
.have_headers:
    mov rdi, r14
    mov rsi, r15
    lea rdx, [rbp-160]
    call parse_request
    test rax, rax
    js .bad_request
    mov [rbp-168], rax
.read_body:
    mov rax, [rbp-168]
    cmp r15, rax
    jae .run
    cmp r15, REQ_SIZE
    jae .bad_request
    mov rdx, REQ_SIZE
    sub rdx, r15
    mov eax, SYS_read
    mov edi, r12d
    lea rsi, [r14+r15]
    syscall
    test rax, rax
    jle .read_body_err
    add r15, rax
    jmp .read_body
.read_body_err:
    cmp rax, EINTR
    je .read_body
    jmp .close
.run:
    mov rdi, r13
    mov rsi, r12
    mov rdx, r14
    lea rcx, [rbp-160]
    mov r8d, [rbp-160+P_CLOSE]
    mov r9, [rbp-176]
    call dispatch
    test rax, rax
    js .close
    cmp dword [rbp-160+P_CLOSE], 0
    jne .close
    mov rax, r15
    sub rax, [rbp-168]
    mov [rbp-184], rax
    test rax, rax
    jle .left_zero
    mov rdi, r14
    mov rsi, r14
    add rsi, [rbp-168]
    mov rdx, rax
    call mem_copy
    mov r15, [rbp-184]
    jmp .read_headers
.left_zero:
    xor r15d, r15d
    jmp .read_headers
.bad_request:
    mov rdi, r12
    mov esi, ST_400
    lea rdx, [rel e_badrequest]
    mov ecx, e_badrequest_len
    mov r8d, 1
    mov r9, [rbp-176]
    call send_response
.close:
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; parse_request(rdi=req, rsi=len, rdx=out) -> rax = total bytes or negative
; -----------------------------------------------------------------------------
parse_request:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov rdi, r14
    xor eax, eax
    mov ecx, 9
    rep stosq
    mov rdi, r12
    mov rsi, r13
    call find_header_end
    cmp rax, -1
    je .bad
    mov r15, rax
    ; request-line end
    mov rdi, r12
    mov rsi, r13
    mov edx, 13
    call find_byte
    cmp rax, -1
    je .bad
    cmp rax, r15
    ja .bad
    mov [rbp-64], rax
    ; method: first space
    mov rdi, r12
    mov rsi, rax
    mov edx, 32
    call find_byte
    cmp rax, -1
    je .bad
    mov rbx, rax
    cmp rbx, 3
    jne .try_post
    mov rdi, r12
    mov rsi, 3
    lea rdx, [rel s_get]
    mov rcx, 3
    call mem_eq
    test eax, eax
    jz .try_post
    mov dword [r14+P_METHOD], M_GET
    jmp .method_done
.try_post:
    cmp rbx, 4
    jne .method_other
    mov rdi, r12
    mov rsi, 4
    lea rdx, [rel s_post]
    mov rcx, 4
    call mem_eq
    test eax, eax
    jz .method_other
    mov dword [r14+P_METHOD], M_POST
    jmp .method_done
.method_other:
    mov dword [r14+P_METHOD], M_OTHER
.method_done:
    ; path start
    lea r8, [rbx+1]
    cmp r8, [rbp-64]
    jae .bad
    mov [rbp-80], r8
    ; second space ends the target
    lea rdi, [r12+r8]
    mov rsi, [rbp-64]
    sub rsi, r8
    mov edx, 32
    call find_byte
    cmp rax, -1
    je .bad
    add rax, r8
    mov [rbp-88], rax
    lea rdx, [r12+r8]
    mov [r14+P_PATH], rdx
    mov rcx, rax
    sub rcx, r8
    mov [r14+P_PATHLEN], rcx
    ; version
    lea r9, [rax+1]
    mov [rbp-96], r9
    lea rdi, [r12+r9]
    mov rsi, [rbp-64]
    sub rsi, r9
    lea rdx, [rel s_http11]
    mov rcx, 8
    call starts_with
    test eax, eax
    jz .ver10
    mov dword [rbp-144], 1
    jmp .ver_done
.ver10:
    lea rdi, [r12+r9]
    mov rsi, [rbp-64]
    sub rsi, r9
    lea rdx, [rel s_http10]
    mov rcx, 8
    call starts_with
    test eax, eax
    jz .bad
    mov dword [rbp-144], 0
.ver_done:
    mov eax, 1
    sub eax, [rbp-144]
    mov [rbp-148], eax
    mov qword [rbp-152], 0
    ; headers
    mov rax, [rbp-64]
    add rax, 2
    mov [rbp-104], rax
.hdr_loop:
    mov rax, [rbp-104]
    cmp rax, r15
    jae .hdr_done
    lea rdi, [r12+rax]
    mov rsi, r13
    sub rsi, rax
    mov edx, 13
    call find_byte
    cmp rax, -1
    je .bad
    add rax, [rbp-104]
    mov [rbp-112], rax
    ; colon
    mov rdi, r12
    add rdi, [rbp-104]
    mov rsi, rax
    sub rsi, [rbp-104]
    mov edx, 58
    call find_byte
    cmp rax, -1
    je .bad
    add rax, [rbp-104]
    mov [rbp-120], rax
    ; value
    lea rdi, [r12+rax+1]
    mov rsi, [rbp-112]
    sub rsi, rax
    dec rsi
    call trim_ws
    mov [rbp-128], rax
    mov [rbp-136], rdx
    ; name
    mov rdi, r12
    add rdi, [rbp-104]
    mov rsi, [rbp-120]
    sub rsi, [rbp-104]
    mov [rbp-160], rsi
    cmp rsi, 14
    jne .c_auth
    lea rdx, [rel s_content_len]
    mov rcx, 14
    call ci_eq
    test eax, eax
    jz .c_auth
    mov rdi, [rbp-128]
    mov rsi, [rbp-136]
    call u64_parse
    cmp rax, -1
    je .bad
    mov [rbp-152], rax
    jmp .hdr_next
.c_auth:
    cmp qword [rbp-160], 13
    jne .c_conn
    mov rdi, r12
    add rdi, [rbp-104]
    mov rsi, 13
    lea rdx, [rel s_auth]
    mov rcx, 13
    call ci_eq
    test eax, eax
    jz .c_conn
    mov rax, [rbp-128]
    mov [r14+P_AUTH], rax
    mov rax, [rbp-136]
    mov [r14+P_AUTHLEN], rax
    jmp .hdr_next
.c_conn:
    cmp qword [rbp-160], 10
    jne .hdr_next
    mov rdi, r12
    add rdi, [rbp-104]
    mov rsi, 10
    lea rdx, [rel s_connection]
    mov rcx, 10
    call ci_eq
    test eax, eax
    jz .hdr_next
    mov rdi, [rbp-128]
    mov rsi, [rbp-136]
    lea rdx, [rel s_close]
    mov rcx, 5
    call ci_eq
    test eax, eax
    jz .conn_keep
    mov dword [rbp-148], 1
    jmp .hdr_next
.conn_keep:
    mov rdi, [rbp-128]
    mov rsi, [rbp-136]
    lea rdx, [rel s_keep]
    mov rcx, 10
    call ci_eq
    test eax, eax
    jz .hdr_next
    mov dword [rbp-148], 0
.hdr_next:
    mov rax, [rbp-112]
    add rax, 2
    mov [rbp-104], rax
    jmp .hdr_loop
.hdr_done:
    mov eax, [rbp-144]
    mov [r14+P_VERSION11], eax
    mov eax, [rbp-148]
    mov [r14+P_CLOSE], eax
    mov rax, [rbp-152]
    mov [r14+P_BODYLEN], rax
    lea rdx, [r15+4]
    mov [r14+P_HEADEREND], rdx
    mov [r14+P_BODYOFF], rdx
    add rax, rdx
    cmp rax, REQ_SIZE
    ja .too_big
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.too_big:
    mov rax, -2
    jmp .out
.bad:
    mov rax, -1
.out:
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; dispatch(rdi=wctx, rsi=fd, rdx=req, rcx=parse, r8d=close, r9=hdrbuf)
; -----------------------------------------------------------------------------
dispatch:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    mov eax, [r15+P_METHOD]
    cmp eax, M_GET
    je .get
    cmp eax, M_POST
    je .post
    jmp .notfound
.get:
    mov r10, [r15+P_PATH]
    mov r11, [r15+P_PATHLEN]
    cmp r11, 7
    jne .get_feed
    mov rdi, r10
    mov rsi, 7
    lea rdx, [rel p_health]
    mov rcx, 7
    call mem_eq
    test eax, eax
    jnz .health
.get_feed:
    cmp r11, 5
    jne .get_post
    mov rdi, r10
    mov rsi, 5
    lea rdx, [rel p_feed]
    mov rcx, 5
    call mem_eq
    test eax, eax
    jnz .feed
.get_post:
    cmp r11, 7
    jbe .notfound
    mov rdi, r10
    mov rsi, r11
    lea rdx, [rel p_posts_slash]
    mov rcx, 7
    call starts_with
    test eax, eax
    jz .notfound
    jmp .get_post_h
.post:
    mov r10, [r15+P_PATH]
    mov r11, [r15+P_PATHLEN]
    cmp r11, 6
    jne .post_like
    mov rdi, r10
    mov rsi, 6
    lea rdx, [rel p_posts]
    mov rcx, 6
    call mem_eq
    test eax, eax
    jnz .create
.post_like:
    cmp r11, 13
    jb .notfound
    mov rdi, r10
    mov rsi, r11
    lea rdx, [rel p_posts_slash]
    mov rcx, 7
    call starts_with
    test eax, eax
    jz .notfound
    lea rdi, [r10+r11-5]
    mov rsi, 5
    lea rdx, [rel p_like]
    mov rcx, 5
    call mem_eq
    test eax, eax
    jz .notfound
    jmp .like
.health:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r15
    mov rcx, [r12+W_RESP]
    mov r8, r14
    call handle_health
    jmp .done
.feed:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r15
    mov rcx, [r12+W_RESP]
    mov r8, r14
    call handle_feed
    jmp .done
.get_post_h:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r15
    mov rcx, [r12+W_RESP]
    mov r8, r14
    call handle_get_post
    jmp .done
.create:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r15
    mov rcx, [r12+W_RESP]
    mov r8, r14
    call handle_create
    jmp .done
.like:
    mov rdi, r12
    mov rsi, r13
    mov rdx, r15
    mov rcx, [r12+W_RESP]
    mov r8, r14
    call handle_like
    jmp .done
.notfound:
    mov rdi, r13
    mov esi, ST_404
    lea rdx, [rel e_notfound]
    mov ecx, e_notfound_len
    mov r8, r15
    mov r9, [r12+W_RESP]
    call send_parse_error
    xor eax, eax
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; send_response(rdi=fd, esi=status, rdx=body, rcx=bodylen, r8d=close, r9=hdrbuf)
; -----------------------------------------------------------------------------
send_response:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 72
    mov r12, rdi
    mov r13d, esi
    mov r14, rdx
    mov r15, rcx
    mov ebx, r8d
    mov [rbp-56], r9
    mov rdi, r9
    lea rax, [rel status_ptrs]
    mov rsi, [rax+r13*8]
    lea rax, [rel status_lens]
    mov edx, [rax+r13*4]
    call append_bytes
    mov rdi, rax
    lea rsi, [rel s_hdr_type]
    mov edx, s_hdr_type_len
    call append_bytes
    mov rdi, rax
    mov rsi, r15
    call append_u64
    mov rdi, rax
    lea rsi, [rel s_hdr_conn]
    mov edx, s_hdr_conn_len
    call append_bytes
    mov rdi, rax
    test ebx, ebx
    jz .keep
    lea rsi, [rel s_close]
    mov edx, 5
    jmp .conn_append
.keep:
    lea rsi, [rel s_keep]
    mov edx, 10
.conn_append:
    call append_bytes
    mov rdi, rax
    lea rsi, [rel s_hdr_end]
    mov edx, s_hdr_end_len
    call append_bytes
    sub rax, [rbp-56]
    mov rdx, rax
    mov rdi, r12
    mov rsi, [rbp-56]
    mov rcx, r14
    mov r8, r15
    call write_iovecs
    add rsp, 72
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; send_parse_error(rdi=fd, esi=status, rdx=msg, rcx=len, r8=parse, r9=hdrbuf)
; -----------------------------------------------------------------------------
send_parse_error:
    mov r10d, [r8+P_CLOSE]
    mov r8d, r10d
    jmp send_response

; -----------------------------------------------------------------------------
; write_iovecs(rdi=fd, rsi=h1, rdx=l1, rcx=h2, r8=l2) -> rax=0/-1
; -----------------------------------------------------------------------------
write_iovecs:
    push rbp
    mov rbp, rsp
    sub rsp, 40
    mov [rsp], rsi
    mov [rsp+8], rdx
    mov [rsp+16], rcx
    mov [rsp+24], r8
    mov r9d, edi
.loop:
    mov rax, [rsp+8]
    add rax, [rsp+24]
    jz .ok
    mov eax, SYS_writev
    mov edi, r9d
    lea rsi, [rsp]
    mov edx, 2
    syscall
    test rax, rax
    js .err
    jz .err
    mov r10, rax
    mov r11, [rsp+8]
    cmp r11, r10
    ja .part0
    sub r10, r11
    mov qword [rsp+8], 0
    add [rsp], r11
    jmp .part1
.part0:
    add [rsp], r10
    sub [rsp+8], r10
    jmp .loop
.part1:
    mov r11, [rsp+24]
    cmp r11, r10
    ja .part1b
    sub r10, r11
    mov qword [rsp+24], 0
    add [rsp+16], r11
    jmp .loop
.part1b:
    add [rsp+16], r10
    sub [rsp+24], r10
    jmp .loop
.err:
    cmp rax, EINTR
    je .loop
    mov eax, -1
    leave
    ret
.ok:
    xor eax, eax
    leave
    ret

; -----------------------------------------------------------------------------
; handle_health(rdi=wctx, rsi=fd, rdx=parse, rcx=hdrbuf, r8=req)
; -----------------------------------------------------------------------------
handle_health:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 40
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    mov rdi, r12
    call db_health
    test eax, eax
    jnz .degraded
    mov rbx, [r12+W_RESP]
    add rbx, HEADER_RESERVE
    mov rdi, rbx
    lea rsi, [rel b_health_open]
    mov edx, b_health_open_len
    call append_bytes
    mov [rbp-56], rax
    mov eax, SYS_clock_gettime
    mov edi, CLOCK_MONOTONIC
    lea rsi, [rbp-80]
    syscall
    mov rax, [rbp-80]
    mov rcx, 1000000000
    imul rax, rcx
    add rax, [rbp-72]
    mov rcx, [rel g_start_sec]
    imul rcx, rcx, 1000000000
    add rcx, [rel g_start_nsec]
    sub rax, rcx
    xor edx, edx
    mov rcx, 1000000000
    div rcx
    mov rdi, [rbp-56]
    mov rsi, rax
    call append_u64
    mov byte [rax], '}'
    inc rax
    mov rcx, rax
    sub rcx, rbx
    mov rdi, r13
    mov esi, ST_200
    mov rdx, rbx
    mov r8d, [r14+P_CLOSE]
    mov r9, r15
    call send_response
    xor eax, eax
    jmp .out
.degraded:
    mov rdi, r13
    mov esi, ST_503
    lea rdx, [rel b_health_bad]
    mov ecx, b_health_bad_len
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
.out:
    add rsp, 40
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; handle_feed(rdi=wctx, rsi=fd, rdx=parse, rcx=hdrbuf, r8=req)
; -----------------------------------------------------------------------------
handle_feed:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    mov rbx, [r12+W_RESP]
    add rbx, HEADER_RESERVE
    mov rdi, r12
    mov rsi, rbx
    mov edx, RESP_SIZE - HEADER_RESERVE
    call db_feed
    cmp rax, -1
    je .err
    mov rcx, rax
    mov rdi, r13
    mov esi, ST_200
    mov rdx, rbx
    mov r8d, [r14+P_CLOSE]
    mov r9, r15
    call send_response
    xor eax, eax
    jmp .out
.err:
    mov rdi, r13
    mov esi, ST_500
    lea rdx, [rel e_internal]
    mov ecx, e_internal_len
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; handle_get_post(rdi=wctx, rsi=fd, rdx=parse, rcx=hdrbuf, r8=req)
; -----------------------------------------------------------------------------
handle_get_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    mov rdi, [r14+P_PATH]
    add rdi, 7
    mov rsi, [r14+P_PATHLEN]
    sub rsi, 7
    call u64_parse
    cmp rax, -1
    je .badid
    test rax, rax
    jz .badid
    mov rbx, rax
    mov rdi, r12
    mov rsi, rbx
    mov rdx, [r12+W_RESP]
    add rdx, HEADER_RESERVE
    mov ecx, RESP_SIZE - HEADER_RESERVE
    call db_get_post
    cmp eax, 0
    je .found
    cmp eax, 1
    je .notfound
    jmp .err
.found:
    mov rcx, rdx
    mov rdi, r13
    mov esi, ST_200
    mov rdx, [r12+W_RESP]
    add rdx, HEADER_RESERVE
    mov r8d, [r14+P_CLOSE]
    mov r9, r15
    call send_response
    xor eax, eax
    jmp .out
.notfound:
    mov rdi, r13
    mov esi, ST_404
    lea rdx, [rel e_postnotfound]
    mov ecx, e_postnotfound_len
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
    jmp .out
.badid:
    mov rdi, r13
    mov esi, ST_400
    lea rdx, [rel e_badid]
    mov ecx, e_badid_len
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
    jmp .out
.err:
    mov rdi, r13
    mov esi, ST_500
    lea rdx, [rel e_internal]
    mov ecx, e_internal_len
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; handle_create(rdi=wctx, rsi=fd, rdx=parse, rcx=hdrbuf, r8=req)
; -----------------------------------------------------------------------------
handle_create:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    mov [rbp-168], r8             ; req
    ; auth first
    mov rdi, [rel g_jwt_secret]
    mov rsi, [rel g_jwt_secret_len]
    mov rdx, [r14+P_AUTH]
    mov rcx, [r14+P_AUTHLEN]
    lea r8, [rbp-96]
    mov r9, [r12+W_USER]
    call jwt_auth
    test eax, eax
    jz .auth_ok
    cmp eax, 1
    je .missing
    cmp eax, 2
    je .invalid
    jmp .payload
.auth_ok:
    mov rax, [rbp-168]
    add rax, [r14+P_BODYOFF]
    mov rdi, rax
    mov rsi, [r14+P_BODYLEN]
    mov rdx, [r12+W_TMP]
    mov ecx, TMP_SIZE
    call json_parse_body
    cmp eax, 0
    je .body_ok
    cmp eax, 1
    je .malformed
    cmp eax, 4
    je .toolong
    jmp .required
.body_ok:
    mov [rbp-184], rdx            ; body ptr
    mov [rbp-176], r8             ; body len
    mov rax, [rbp-96+J_UID]
    mov [rbp-160+C_UID], rax
    mov rax, [rbp-184]
    mov [rbp-160+C_BODY], rax
    mov rax, [rbp-176]
    mov [rbp-160+C_BODYLEN], rax
    mov rax, [r12+W_USER]
    mov [rbp-160+C_UNAME], rax
    mov rax, [rbp-96+J_UNAMELEN]
    mov [rbp-160+C_UNAMELEN], rax
    mov rax, [r12+W_RESP]
    add rax, HEADER_RESERVE
    mov [rbp-160+C_DST], rax
    mov qword [rbp-160+C_OUTLEN], 0
    mov rdi, r12
    lea rsi, [rbp-160]
    call db_create_post
    test eax, eax
    jnz .err
    mov rcx, [rbp-160+C_OUTLEN]
    mov rdi, r13
    mov esi, ST_201
    mov rdx, [r12+W_RESP]
    add rdx, HEADER_RESERVE
    mov r8d, [r14+P_CLOSE]
    mov r9, r15
    call send_response
    xor eax, eax
    jmp .out
.missing:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_missingbearer]
    mov ecx, e_missingbearer_len
    jmp .send_err
.invalid:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_invalidtoken]
    mov ecx, e_invalidtoken_len
    jmp .send_err
.payload:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_invalidpayload]
    mov ecx, e_invalidpayload_len
    jmp .send_err
.malformed:
    mov rdi, r13
    mov esi, ST_400
    lea rdx, [rel e_malformedjson]
    mov ecx, e_malformedjson_len
    jmp .send_err
.required:
    mov rdi, r13
    mov esi, ST_400
    lea rdx, [rel e_bodyrequired]
    mov ecx, e_bodyrequired_len
    jmp .send_err
.toolong:
    mov rdi, r13
    mov esi, ST_400
    lea rdx, [rel e_bodylong]
    mov ecx, e_bodylong_len
    jmp .send_err
.err:
    mov rdi, r13
    mov esi, ST_500
    lea rdx, [rel e_internal]
    mov ecx, e_internal_len
.send_err:
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
.out:
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; handle_like(rdi=wctx, rsi=fd, rdx=parse, rcx=hdrbuf, r8=req)
; -----------------------------------------------------------------------------
handle_like:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 88
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, rcx
    ; auth first
    mov rdi, [rel g_jwt_secret]
    mov rsi, [rel g_jwt_secret_len]
    mov rdx, [r14+P_AUTH]
    mov rcx, [r14+P_AUTHLEN]
    lea r8, [rbp-96]
    mov r9, [r12+W_USER]
    call jwt_auth
    test eax, eax
    jz .auth_ok
    cmp eax, 1
    je .missing
    cmp eax, 2
    je .invalid
    jmp .payload
.auth_ok:
    mov rdi, [r14+P_PATH]
    add rdi, 7
    mov rsi, [r14+P_PATHLEN]
    sub rsi, 12
    call u64_parse
    cmp rax, -1
    je .badid
    test rax, rax
    jz .badid
    mov rbx, rax
    mov rdi, r12
    mov rsi, [rbp-96+J_UID]
    mov rdx, rbx
    call db_like
    cmp eax, 0
    je .inserted
    cmp eax, 1
    je .already
    cmp eax, 2
    je .notfound
    jmp .err
.inserted:
    lea rsi, [rel b_like_new]
    mov edx, b_like_new_len
    mov dword [rbp-120], ST_201
    jmp .render
.already:
    lea rsi, [rel b_like_old]
    mov edx, b_like_old_len
    mov dword [rbp-120], ST_200
.render:
    mov rdi, [r12+W_RESP]
    add rdi, HEADER_RESERVE
    mov [rbp-112], rdi
    call append_bytes
    mov rdi, rax
    mov rsi, rbx
    call append_u64
    mov byte [rax], '}'
    inc rax
    mov rcx, rax
    sub rcx, [rbp-112]
    mov rdi, r13
    mov esi, [rbp-120]
    mov rdx, [rbp-112]
    mov r8d, [r14+P_CLOSE]
    mov r9, r15
    call send_response
    xor eax, eax
    jmp .out
.notfound:
    mov rdi, r13
    mov esi, ST_404
    lea rdx, [rel e_postnotfound]
    mov ecx, e_postnotfound_len
    jmp .send_err
.badid:
    mov rdi, r13
    mov esi, ST_400
    lea rdx, [rel e_badid]
    mov ecx, e_badid_len
    jmp .send_err
.missing:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_missingbearer]
    mov ecx, e_missingbearer_len
    jmp .send_err
.invalid:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_invalidtoken]
    mov ecx, e_invalidtoken_len
    jmp .send_err
.payload:
    mov rdi, r13
    mov esi, ST_401
    lea rdx, [rel e_invalidpayload]
    mov ecx, e_invalidpayload_len
    jmp .send_err
.err:
    mov rdi, r13
    mov esi, ST_500
    lea rdx, [rel e_internal]
    mov ecx, e_internal_len
.send_err:
    mov r8, r14
    mov r9, r15
    call send_parse_error
    xor eax, eax
.out:
    add rsp, 88
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
