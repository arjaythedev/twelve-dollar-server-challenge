; Feed API server: raw Linux syscalls for sockets/HTTP, SQLite through its C
; API. A single-threaded epoll event loop owns every connection (Nginx mode),
; with non-blocking reads and blocking writes, one SQLite handle, and shared
; request/response scratch buffers.

%include "common.inc"

extern malloc
extern free
extern realloc
extern getenv
extern strlen
extern atoi
extern inet_pton
extern signal

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
msg_db: db 'feed: cannot initialise database',10
msg_db_len equ $ - msg_db

section .bss
g_listen_fd:     resd 1
g_epfd:          resd 1
g_jwt_secret:    resq 1
g_jwt_secret_len: resq 1
g_start_sec:     resq 1
g_start_nsec:    resq 1
g_timeout:       resq 2
g_now_ms:        resq 1
g_last_sweep:    resq 1
sockaddr_in:     resb 16
evtmp:           resb 16
g_wctx:          resb W_SIZE
conns:           resb CONN_SIZE * MAX_CONN
evbuf:           resb 12 * MAX_EVENTS

section .text

global main

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
    mov esi, 4096
    syscall
    test rax, rax
    js .bad_listen

    ; the listening socket never blocks: accept() drains, then reports EAGAIN
    mov eax, SYS_fcntl
    mov edi, r12d
    mov esi, F_SETFL
    mov edx, O_NONBLOCK
    syscall

    mov eax, SYS_epoll_create1
    xor edi, edi
    syscall
    test rax, rax
    js .bad_listen
    mov [rel g_epfd], eax

    mov dword [rel evtmp], EPOLLIN
    mov [rel evtmp+4], r12
    mov eax, SYS_epoll_ctl
    mov edi, [rel g_epfd]
    mov esi, EPOLL_CTL_ADD
    mov edx, r12d
    lea r10, [rel evtmp]
    syscall
    test rax, rax
    js .bad_listen

    ; one SQLite connection for the whole process
    lea rdi, [rel g_wctx]
    call db_init
    test eax, eax
    jnz .bad_db

    mov edi, RESP_SIZE
    call malloc
    test rax, rax
    jz .bad_db
    mov [rel g_wctx+W_RESP], rax
    mov edi, TMP_SIZE
    call malloc
    test rax, rax
    jz .bad_db
    mov [rel g_wctx+W_TMP], rax
    mov edi, USER_SIZE
    call malloc
    test rax, rax
    jz .bad_db
    mov [rel g_wctx+W_USER], rax

    call now_ms
    mov [rel g_now_ms], rax
    mov [rel g_last_sweep], rax
    call event_loop             ; never returns

.bad_listen:
    lea rdi, [rel msg_listen]
    mov esi, msg_listen_len
    call die
.bad_db:
    lea rdi, [rel msg_db]
    mov esi, msg_db_len
    call die
.no_db:
    lea rdi, [rel msg_no_db]
    mov esi, msg_no_db_len
    call die

; -----------------------------------------------------------------------------
; now_ms() -> rax = CLOCK_MONOTONIC milliseconds
; -----------------------------------------------------------------------------
now_ms:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    mov eax, SYS_clock_gettime
    mov edi, CLOCK_MONOTONIC
    mov rsi, rsp
    syscall
    mov rax, [rsp]
    imul rax, rax, 1000
    mov r8, rax
    mov rax, [rsp+8]
    xor edx, edx
    mov rcx, 1000000
    div rcx
    add rax, r8
    leave
    ret

; -----------------------------------------------------------------------------
; event_loop() - epoll_wait, then service connection events, then accepts.
; -----------------------------------------------------------------------------
event_loop:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
.loop:
    mov eax, SYS_epoll_wait
    mov edi, [rel g_epfd]
    lea rsi, [rel evbuf]
    mov edx, MAX_EVENTS
    mov r10d, 1000
    syscall
    test rax, rax
    js .loop                    ; EINTR: just re-enter
    mov r15, rax
    call now_ms
    mov [rel g_now_ms], rax
    mov rcx, [rel g_last_sweep]
    add rcx, SWEEP_INTERVAL_MS
    cmp rax, rcx
    jb .events
    mov [rel g_last_sweep], rax
    call sweep_idle
.events:
    ; connections first, so an accept later in this batch cannot recycle a
    ; descriptor that still has an event queued for it
    xor r14d, r14d
.ev1:
    cmp r14, r15
    jae .phase2
    lea rbx, [rel evbuf]
    lea rax, [r14+r14*2]
    shl rax, 2
    add rbx, rax
    mov r12d, [rbx+4]
    cmp r12d, [rel g_listen_fd]
    je .ev1_next
    mov eax, [rbx]
    test eax, EPOLLIN
    jnz .ev1_read
    test eax, EPOLLOUT
    jnz .ev1_out
    test eax, EPOLLERR | EPOLLHUP | EPOLLRDHUP
    jz .ev1_next
    mov rdi, r12
    call conn_close
    jmp .ev1_next
.ev1_read:
    mov rdi, r12
    call conn_read
    jmp .ev1_next
.ev1_out:
    mov rdi, r12
    call conn_flush
    test eax, eax
    js .ev1_close
    jnz .ev1_next
    mov rdi, r12
    call conn_process
    test eax, eax
    jz .ev1_next
.ev1_close:
    mov rdi, r12
    call conn_close
.ev1_next:
    inc r14
    jmp .ev1
.phase2:
    xor r14d, r14d
.ev2:
    cmp r14, r15
    jae .loop
    lea rbx, [rel evbuf]
    lea rax, [r14+r14*2]
    shl rax, 2
    add rbx, rax
    mov r12d, [rbx+4]
    cmp r12d, [rel g_listen_fd]
    jne .ev2_next
    call accept_new
.ev2_next:
    inc r14
    jmp .ev2

; -----------------------------------------------------------------------------
; accept_new() - drain the accept queue
; -----------------------------------------------------------------------------
accept_new:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 40
.acc:
    mov eax, SYS_accept
    mov edi, [rel g_listen_fd]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    js .out                     ; EAGAIN: queue drained
    mov r13d, eax
    cmp r13d, MAX_CONN
    jae .reject
    ; non-blocking so neither read nor write can stall the event loop
    mov eax, SYS_fcntl
    mov edi, r13d
    mov esi, F_SETFL
    mov edx, O_NONBLOCK
    syscall
    mov eax, SYS_setsockopt
    mov edi, r13d
    mov esi, IPPROTO_TCP
    mov edx, TCP_NODELAY
    lea r10, [rel one]
    mov r8d, 4
    syscall
    mov eax, r13d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    mov edi, CONN_INIT_CAP
    call malloc
    test rax, rax
    jz .reject
    mov [rbx+CONN_BUF], rax
    mov qword [rbx+CONN_LEN], 0
    mov qword [rbx+CONN_CAP], CONN_INIT_CAP
    mov qword [rbx+CONN_OUTLEN], 0
    mov qword [rbx+CONN_OUTOFF], 0
    mov qword [rbx+CONN_OUTBUF], 0
    mov qword [rbx+CONN_OUTCAP], 0
    mov rcx, [rel g_now_ms]
    mov [rbx+CONN_LAST], rcx
    mov dword [rel evtmp], EPOLLIN | EPOLLRDHUP
    mov [rel evtmp+4], r13
    mov eax, SYS_epoll_ctl
    mov edi, [rel g_epfd]
    mov esi, EPOLL_CTL_ADD
    mov edx, r13d
    lea r10, [rel evtmp]
    syscall
    test rax, rax
    js .reject_free
    mov rdi, r13
    call conn_read            ; the first request may already be waiting
    jmp .acc
.reject_free:
    mov rdi, [rbx+CONN_BUF]
    call free
    mov qword [rbx+CONN_BUF], 0
.reject:
    mov eax, SYS_close
    mov edi, r13d
    syscall
    jmp .acc
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
; conn_read(rdi=fd) - drain readable bytes, then run every complete request
; -----------------------------------------------------------------------------
conn_read:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12d, edi
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    cmp qword [rbx+CONN_BUF], 0
    je .out                     ; stale event for an already closed connection
.read:
    mov rdx, [rbx+CONN_CAP]
    sub rdx, [rbx+CONN_LEN]
    jnz .have_space
    ; buffer full: try to drain complete requests before growing
    mov rdi, r12
    call conn_process
    test eax, eax
    jnz .close
    mov rax, [rbx+CONN_CAP]
    cmp [rbx+CONN_LEN], rax
    jb .read
    mov rdi, r12
    call conn_grow
    test eax, eax
    jnz .too_big
    jmp .read
.have_space:
    mov eax, SYS_recvfrom
    mov edi, r12d
    mov rsi, [rbx+CONN_BUF]
    add rsi, [rbx+CONN_LEN]
    mov r10d, MSG_DONTWAIT
    xor r8d, r8d
    xor r9d, r9d
    syscall
    test rax, rax
    jg .got
    je .eof
    cmp rax, EAGAIN
    je .process
    cmp rax, EINTR
    je .read
    jmp .close
.got:
    add [rbx+CONN_LEN], rax
    mov rcx, [rel g_now_ms]
    mov [rbx+CONN_LAST], rcx
    jmp .read
.eof:
    mov rdi, r12
    call conn_process
    jmp .close
.process:
    mov rdi, r12
    call conn_process
    test eax, eax
    jnz .close
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.close:
    mov rdi, r12
    call conn_close
    jmp .out
.too_big:
    mov rdi, r12
    mov esi, ST_400
    lea rdx, [rel e_badrequest]
    mov ecx, e_badrequest_len
    mov r8d, 1
    call send_simple
    jmp .close

; -----------------------------------------------------------------------------
; conn_process(rdi=fd) -> eax = 0 keep / 1 closed
; Runs every fully buffered request, then compacts the leftovers.
; -----------------------------------------------------------------------------
conn_process:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    mov r12d, edi
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    xor r13d, r13d              ; consumed offset
    cmp qword [rbx+CONN_BUF], 0
    je .keep
.next_req:
    cmp qword [rbx+CONN_OUTLEN], 0
    jne .compact                ; response still draining: keep order
    mov r14, [rbx+CONN_LEN]
    sub r14, r13
    jz .compact
    mov rdi, [rbx+CONN_BUF]
    add rdi, r13
    mov rsi, r14
    call find_header_end
    cmp rax, -1
    je .partial
    mov rdi, [rbx+CONN_BUF]
    add rdi, r13
    mov rsi, r14
    lea rdx, [rbp-160]
    call parse_request
    test rax, rax
    js .bad
    mov r15, rax
    mov rcx, r13
    add rcx, r15
    cmp rcx, [rbx+CONN_LEN]
    ja .compact                 ; body still on the wire
    lea rdi, [rel g_wctx]
    mov rsi, r12
    mov rdx, [rbx+CONN_BUF]
    add rdx, r13
    lea rcx, [rbp-160]
    mov r8d, [rbp-160+P_CLOSE]
    mov r9, [rel g_wctx+W_RESP]
    call dispatch
    test rax, rax
    js .close
    cmp dword [rbp-160+P_CLOSE], 0
    jne .close
    add r13, r15
    jmp .next_req
.partial:
    mov rax, [rbx+CONN_LEN]
    sub rax, r13
    cmp rax, [rbx+CONN_CAP]
    jb .compact                 ; wait for more bytes
    jmp .bad                    ; a header this large can never be valid
.compact:
    mov rdx, [rbx+CONN_LEN]
    sub rdx, r13
    jz .clear
    mov rdi, [rbx+CONN_BUF]
    mov rsi, rdi
    add rsi, r13
    call mem_copy
    mov [rbx+CONN_LEN], rdx
    xor eax, eax
    jmp .out
.clear:
    mov qword [rbx+CONN_LEN], 0
.keep:
    xor eax, eax
    jmp .out
.bad:
    mov rdi, r12
    mov esi, ST_400
    lea rdx, [rel e_badrequest]
    mov ecx, e_badrequest_len
    mov r8d, 1
    call send_simple
.close:
    mov rdi, r12
    call conn_close
    mov eax, 1
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
; conn_grow(rdi=fd) -> eax = 0 ok / -1 at the request size limit or OOM
; -----------------------------------------------------------------------------
conn_grow:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12d, edi
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    mov rax, [rbx+CONN_CAP]
    cmp rax, REQ_SIZE
    jae .fail
    shl rax, 1
    cmp rax, REQ_SIZE
    jbe .size_ok
    mov eax, REQ_SIZE
.size_ok:
    mov r13, rax
    mov rdi, [rbx+CONN_BUF]
    mov rsi, rax
    call realloc
    test rax, rax
    jz .fail
    mov [rbx+CONN_BUF], rax
    mov [rbx+CONN_CAP], r13
    xor eax, eax
    jmp .out
.fail:
    mov eax, -1
.out:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; conn_close(rdi=fd) - idempotent: deregister, close, free
; -----------------------------------------------------------------------------
conn_close:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12d, edi
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    mov rdi, [rbx+CONN_BUF]
    test rdi, rdi
    jz .out
    mov eax, SYS_epoll_ctl
    mov edi, [rel g_epfd]
    mov esi, EPOLL_CTL_DEL
    mov edx, r12d
    xor r10d, r10d
    syscall
    mov eax, SYS_close
    mov edi, r12d
    syscall
    mov rdi, [rbx+CONN_BUF]
    call free
    mov qword [rbx+CONN_BUF], 0
    mov qword [rbx+CONN_LEN], 0
    mov qword [rbx+CONN_CAP], 0
    mov rdi, [rbx+CONN_OUTBUF]
    test rdi, rdi
    jz .no_out
    call free
    mov qword [rbx+CONN_OUTBUF], 0
    mov qword [rbx+CONN_OUTCAP], 0
.no_out:
    mov qword [rbx+CONN_OUTOFF], 0
    mov qword [rbx+CONN_OUTLEN], 0
.out:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; sweep_idle() - close connections with no traffic for IDLE_TIMEOUT_MS
; -----------------------------------------------------------------------------
sweep_idle:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    xor r12d, r12d
.loop:
    cmp r12d, MAX_CONN
    jae .out
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    mov rax, [rbx+CONN_BUF]
    test rax, rax
    jz .next
    mov rax, [rel g_now_ms]
    sub rax, [rbx+CONN_LAST]
    cmp rax, IDLE_TIMEOUT_MS
    jb .next
    mov rdi, r12
    call conn_close
.next:
    inc r12
    jmp .loop
.out:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; send_simple(rdi=fd, esi=status, rdx=body, rcx=len, r8d=close)
; -----------------------------------------------------------------------------
send_simple:
    mov r9, [rel g_wctx+W_RESP]
    jmp send_response


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
; write_iovecs(rdi=fd, rsi=h1, rdx=l1, rcx=h2, r8=l2)
;   -> rax = 0 fully sent / 1 queued for EPOLLOUT / -1 error
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
    js .werr
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
.werr:
    cmp rax, EAGAIN
    je .queue
    cmp rax, EINTR
    je .loop
    jmp .err
.queue:
    ; send buffer is full: keep the remainder for EPOLLOUT
    mov edi, r9d
    mov rsi, [rsp]
    mov rdx, [rsp+8]
    mov rcx, [rsp+16]
    mov r8, [rsp+24]
    call conn_queue
    leave
    ret
.ok:
    xor eax, eax
    leave
    ret
.err:
    mov eax, -1
    leave
    ret

; -----------------------------------------------------------------------------
; conn_queue(rdi=fd, rsi=b0, rdx=l0, rcx=b1, r8=l1) -> rax = 1 queued / -1 error
; Only called when nothing is queued yet.
; -----------------------------------------------------------------------------
conn_queue:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 40
    mov r12d, edi
    mov [rsp], rsi
    mov [rsp+8], rdx
    mov [rsp+16], rcx
    mov [rsp+24], r8
    add rdx, r8
    mov r13, rdx                ; total bytes
    jz .ok
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    cmp [rbx+CONN_OUTCAP], r13
    jae .have_buf
    mov rdi, [rbx+CONN_OUTBUF]
    mov rsi, r13
    add rsi, rsi                ; grow to twice the need
    call realloc
    test rax, rax
    jz .err
    mov [rbx+CONN_OUTBUF], rax
    mov rcx, r13
    add rcx, rcx
    mov [rbx+CONN_OUTCAP], rcx
.have_buf:
    mov rdi, [rbx+CONN_OUTBUF]
    mov rsi, [rsp]
    mov rdx, [rsp+8]
    call mem_copy
    mov rdi, [rbx+CONN_OUTBUF]
    add rdi, [rsp+8]
    mov rsi, [rsp+16]
    mov rdx, [rsp+24]
    call mem_copy
    mov qword [rbx+CONN_OUTOFF], 0
    mov [rbx+CONN_OUTLEN], r13
    mov rdi, r12
    mov esi, EPOLLOUT | EPOLLRDHUP
    call set_events
    test eax, eax
    js .err
    mov eax, 1
    jmp .out
.ok:
    xor eax, eax
    jmp .out
.err:
    mov eax, -1
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
; conn_flush(rdi=fd) -> eax = 0 drained / 1 still pending / -1 error
; -----------------------------------------------------------------------------
conn_flush:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12d, edi
    mov eax, r12d
    imul rax, rax, CONN_SIZE
    lea rbx, [rel conns]
    add rbx, rax
    cmp qword [rbx+CONN_BUF], 0
    je .out
.flush:
    mov rdx, [rbx+CONN_OUTLEN]
    test rdx, rdx
    jz .drained
    mov eax, SYS_write
    mov edi, r12d
    mov rsi, [rbx+CONN_OUTBUF]
    add rsi, [rbx+CONN_OUTOFF]
    syscall
    test rax, rax
    jg .wrote
    je .err
    cmp rax, EAGAIN
    je .pending
    cmp rax, EINTR
    je .flush
    jmp .err
.wrote:
    add [rbx+CONN_OUTOFF], rax
    sub [rbx+CONN_OUTLEN], rax
    mov rcx, [rel g_now_ms]
    mov [rbx+CONN_LAST], rcx
    jmp .flush
.drained:
    mov qword [rbx+CONN_OUTOFF], 0
    mov rdi, r12
    mov esi, EPOLLIN | EPOLLRDHUP
    call set_events
    test eax, eax
    js .err
    xor eax, eax
    jmp .out
.pending:
    mov eax, 1
    jmp .out
.err:
    mov eax, -1
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
; set_events(rdi=fd, esi=mask) -> eax = 0 / -1
; -----------------------------------------------------------------------------
set_events:
    push rbp
    mov rbp, rsp
    mov [rel evtmp], esi
    mov [rel evtmp+4], rdi
    mov rdx, rdi
    mov rsi, EPOLL_CTL_MOD
    lea r10, [rel evtmp]
    mov edi, [rel g_epfd]
    mov eax, SYS_epoll_ctl
    syscall
    test rax, rax
    js .err
    xor eax, eax
    leave
    ret
.err:
    mov eax, -1
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
