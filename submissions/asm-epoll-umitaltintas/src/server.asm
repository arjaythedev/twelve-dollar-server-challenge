; The $12 server challenge in x86-64 assembly (NASM, Linux).
;
; One thread, one epoll loop, one SQLite connection, raw syscalls for all I/O.
; Each epoll batch is processed like this:
;   1. read every ready socket and parse/handle the requests in it
;   2. writes (POST /posts, likes) run inside one transaction opened by the first write
;   3. every response of the batch is built into one arena
;   4. COMMIT, then send all responses (so nobody sees a write before it is committed)
; SQLite is the only C code (built from the pinned amalgamation); libc is linked because SQLite needs it.

default rel
bits 64

%include "defs.inc"

extern getenv, malloc, realloc, free, time, exit, strlen, atoi, pthread_create, usleep
extern sqlite3_initialize, sqlite3_open_v2, sqlite3_exec, sqlite3_prepare_v3, sqlite3_step
extern sqlite3_reset, sqlite3_bind_int64, sqlite3_bind_text, sqlite3_column_int64
extern sqlite3_column_text, sqlite3_column_bytes, sqlite3_changes, sqlite3_errmsg
extern sqlite3_wal_checkpoint_v2, sqlite3_wal_hook

global main

section .bss
alignb 64
conns:      resb MAXCONN << CONN_SHIFT
events:     resb MAXEV * 12
recs:       resb MAXREC * 16
scratch:    resb SCRATCH_SZ + 64
bodybuf:    resb BODY_SZ
strbuf:     resb SCRATCH_SZ       ; decoded POST body
hdrbuf:     resb 1024             ; decoded JWT header
plbuf:      resb 4096             ; decoded JWT payload
subbuf:     resb 4096
namebuf:    resb 4096             ; decoded username
arena:      resb ARENA_SZ

alignb 8
db:         resq 1
ckpt_db:    resq 1
st_feed:    resq 1
st_post:    resq 1
st_ins:     resq 1
st_like:    resq 1
st_exists:  resq 1
st_one:     resq 1
st_begin:   resq 1
st_commit:  resq 1
start_time: resq 1
now:        resq 1
arena_used: resq 1
nrec:       resd 1
epfd:       resd 1
lfd:        resd 1
cur_fd:     resd 1
cur_flags:  resd 1                ; RF_* for the response being built
nconn:      resd 1                ; open client connections
ipcnt:      resd IP_BUCKETS       ; open connections per client-address bucket
acc_addr:   resb 16               ; sockaddr_in from accept4
acc_alen:   resd 1
accept_off: resd 1                ; 1 while the listen socket is out of epoll (fd table full)
in_txn:     resd 1
wal_frames: resd 1                ; frames in the WAL after the last commit (from the WAL hook)
req_method: resd 1                ; 1 GET, 2 POST, 0 other
req_path:   resq 1
req_plen:   resq 1
req_body:   resq 1
req_blen:   resq 1
req_auth:   resq 1
req_alen:   resq 1
jwt_uid:    resq 1
jwt_name:   resq 1
jwt_nlen:   resq 1
epev:       resb 16
db_path:    resq 1

section .data
align 8
keytab_hdr:  dd 1, 0
             dq k_alg, 3, 0
keytab_pl:   dd 3, 0
             dq k_sub, 3, 0
             dq k_username, 8, 0
             dq k_exp, 3, 0
keytab_body: dd 1, 0
             dq k_body, 4, 0
%define HDR_ALG   keytab_hdr+8+16
%define PL_SUB    keytab_pl+8+16
%define PL_NAME   keytab_pl+8+24+16
%define PL_EXP    keytab_pl+8+48+16
%define BODY_VAL  keytab_body+8+16

one:         dd 1
sockaddr:    dw 2                 ; AF_INET
sa_port:     dw 0
sa_addr:     dd 0
             dq 0

section .rodata
k_alg:       db 'alg'
k_sub:       db 'sub'
k_username:  db 'username'
k_exp:       db 'exp'
k_body:      db 'body'

s_200: db `HTTP/1.1 200 OK\r\n`
s_201: db `HTTP/1.1 201 Created\r\n`
s_400: db `HTTP/1.1 400 Bad Request\r\n`
s_401: db `HTTP/1.1 401 Unauthorized\r\n`
s_404: db `HTTP/1.1 404 Not Found\r\n`
s_500: db `HTTP/1.1 500 Internal Server Error\r\n`
s_503: db `HTTP/1.1 503 Service Unavailable\r\n`
s_end:
align 8
status_tab:
  dq s_200, s_201 - s_200
  dq s_201, s_400 - s_201
  dq s_400, s_401 - s_400
  dq s_401, s_404 - s_401
  dq s_404, s_500 - s_404
  dq s_500, s_503 - s_500
  dq s_503, s_end - s_503

env_path:    db 'SQLITE_PATH', 0
env_secret:  db 'JWT_SECRET', 0
env_host:    db 'HOST', 0
env_port:    db 'PORT', 0
def_secret:  db 'twelve-dollar-challenge', 0

sql_pragmas: db 'PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA mmap_size=1073741824;'
             db 'PRAGMA cache_size=-8000;PRAGMA temp_store=MEMORY;PRAGMA busy_timeout=5000;'
             db 'PRAGMA wal_autocheckpoint=0;PRAGMA journal_size_limit=67108864;', 0
sql_ckpt_pragmas: db 'PRAGMA busy_timeout=5000;PRAGMA mmap_size=1073741824;', 0
sql_feed:    db 'SELECT p.id,p.body,p.created_at,u.username,'
             db '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
             db 'FROM posts p JOIN users u ON u.id=p.user_id '
             db 'ORDER BY p.created_at DESC,p.id DESC LIMIT 20', 0
sql_post:    db 'SELECT p.id,p.body,p.created_at,u.username,'
             db '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
             db 'FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1', 0
sql_ins:     db 'INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at', 0
sql_like:    db 'INSERT INTO likes(user_id,post_id) SELECT ?1,?2 '
             db 'WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT DO NOTHING', 0
sql_exists:  db 'SELECT 1 FROM posts WHERE id=?1', 0
sql_one:     db 'SELECT 1', 0
sql_begin:   db 'BEGIN', 0
sql_commit:  db 'COMMIT', 0
sql_rollback: db 'ROLLBACK', 0
sql_warm:    db 'SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx;'
             db 'SELECT count(*) FROM likes INDEXED BY likes_post_id_idx;'
             db 'SELECT count(*) FROM users;', 0

msg_nopath:  db `SQLITE_PATH is not set\n`, 0
msg_dberr:   db `cannot open or prepare the database: `, 0
msg_neterr:  db `cannot listen on HOST:PORT\n`, 0
msg_ready:   db `asm server listening\n`, 0
nl:          db `\n`, 0

section .text
%include "sha256.inc"
%include "json.inc"
section .text

; ---------------------------------------------------------------------------
; small helpers

; puts2(rdi = C string) to stderr
puts2:
  push rbx
  mov rbx, rdi
  call strlen
  mov rdx, rax
  mov rsi, rbx
  mov edi, 2
  SYSCALL SYS_write
  pop rbx
  ret

fatal_db:
  lea rdi, [msg_dberr]
  call puts2
  mov rdi, [db]
  call sqlite3_errmsg
  mov rdi, rax
  call puts2
  lea rdi, [nl]
  call puts2
  mov edi, 1
  call exit

; prep(rdi = sql) -> rax = statement (persistent)
prep:
  push rbx
  sub rsp, 16
  mov rsi, rdi
  mov rdi, [db]
  mov edx, -1
  mov ecx, 1                  ; SQLITE_PREPARE_PERSISTENT
  lea r8, [rsp]
  xor r9d, r9d
  call sqlite3_prepare_v3
  test eax, eax
  jnz fatal_db
  mov rax, [rsp]
  add rsp, 16
  pop rbx
  ret

; ep_ctl(edi = op, esi = fd, edx = events)
ep_ctl:
  mov [epev], edx
  mov [epev+4], esi
  mov dword [epev+8], 0
  mov edx, esi
  mov esi, edi
  mov edi, [epfd]
  lea r10, [epev]
  SYSCALL SYS_epoll_ctl
  ret

; conn_ptr: eax = fd -> rax = &conns[fd]
%macro CONN 2                      ; dest reg, fd reg (32-bit)
  mov %1, %2
  shl %1, CONN_SHIFT
  lea r11, [conns]
  add %1, r11
%endmacro

; ---------------------------------------------------------------------------
; startup

main:
  push rbp
  mov rbp, rsp
  push rbx
  push r12

  call b64_init
  xor edi, edi
  call time
  mov [start_time], rax
  mov [now], rax

  lea rdi, [env_secret]
  call getenv
  test rax, rax
  jnz .secret
  lea rax, [def_secret]
.secret:
  mov rbx, rax
  mov rdi, rax
  call strlen
  mov rsi, rax
  mov rdi, rbx
  call hmac_init

  lea rdi, [env_path]
  call getenv
  test rax, rax
  jnz .path
  lea rdi, [msg_nopath]
  call puts2
  mov edi, 1
  call exit
.path:
  mov [db_path], rax
  call db_init

  ; HOST (dotted quad, default 0.0.0.0) and PORT (default 3000)
  lea rdi, [env_host]
  call getenv
  xor ebx, ebx
  test rax, rax
  jz .port
  mov rsi, rax
  xor ecx, ecx                ; shift
  xor edx, edx                ; current octet
.oct:
  movzx eax, byte [rsi]
  inc rsi
  cmp al, '.'
  je .dot
  test al, al
  jz .last
  sub al, '0'
  imul edx, edx, 10
  add edx, eax
  jmp .oct
.dot:
  shl edx, cl
  or ebx, edx
  xor edx, edx
  add ecx, 8
  jmp .oct
.last:
  shl edx, cl
  or ebx, edx
.port:
  mov [sa_addr], ebx
  lea rdi, [env_port]
  call getenv
  mov r12d, 3000
  test rax, rax
  jz .haveport
  mov rdi, rax
  call atoi
  mov r12d, eax
.haveport:
  mov eax, r12d
  xchg al, ah
  mov [sa_port], ax
  call net_init

  ; checkpoints run on their own thread and connection, off the request path
  sub rsp, 16
  lea rdi, [rsp]
  xor esi, esi
  lea rdx, [ckpt_thread]
  xor ecx, ecx
  call pthread_create
  add rsp, 16

  lea rdi, [msg_ready]
  call puts2
  call event_loop
  xor eax, eax
  pop r12
  pop rbx
  pop rbp
  ret

db_init:
  push rbx
  call sqlite3_initialize
  mov rdi, [db_path]
  lea rsi, [db]
  mov edx, 0x8002             ; READWRITE | NOMUTEX
  xor ecx, ecx
  call sqlite3_open_v2
  test eax, eax
  jnz fatal_db
  mov rdi, [db]
  lea rsi, [sql_pragmas]
  xor edx, edx
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_exec
  test eax, eax
  jnz fatal_db
  mov rdi, [db]
  lea rsi, [wal_cb]
  xor edx, edx
  call sqlite3_wal_hook
  ; read the hot indexes once so the first requests hit the OS page cache
  mov rdi, [db]
  lea rsi, [sql_warm]
  xor edx, edx
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_exec
  lea rdi, [sql_feed]
  call prep
  mov [st_feed], rax
  lea rdi, [sql_post]
  call prep
  mov [st_post], rax
  lea rdi, [sql_ins]
  call prep
  mov [st_ins], rax
  lea rdi, [sql_like]
  call prep
  mov [st_like], rax
  lea rdi, [sql_exists]
  call prep
  mov [st_exists], rax
  lea rdi, [sql_one]
  call prep
  mov [st_one], rax
  lea rdi, [sql_begin]
  call prep
  mov [st_begin], rax
  lea rdi, [sql_commit]
  call prep
  mov [st_commit], rax
  pop rbx
  ret

net_init:
  push rbx
  mov edi, 2                  ; AF_INET
  mov esi, 1 | SOCK_NONBLOCK  ; SOCK_STREAM
  xor edx, edx
  SYSCALL SYS_socket
  test eax, eax
  js .err
  mov [lfd], eax
  mov edi, eax
  mov esi, 1                  ; SOL_SOCKET
  mov edx, 2                  ; SO_REUSEADDR
  lea r10, [one]
  mov r8d, 4
  SYSCALL SYS_setsockopt
  mov edi, [lfd]
  lea rsi, [sockaddr]
  mov edx, 16
  SYSCALL SYS_bind
  test eax, eax
  jnz .err
  mov edi, [lfd]
  mov esi, 65535
  SYSCALL SYS_listen
  test eax, eax
  jnz .err
  xor edi, edi
  SYSCALL SYS_epoll_create1
  test eax, eax
  js .err
  mov [epfd], eax
  mov edi, EPOLL_CTL_ADD
  mov esi, [lfd]
  mov edx, EPOLLIN
  call ep_ctl
  pop rbx
  ret
.err:
  lea rdi, [msg_neterr]
  call puts2
  mov edi, 1
  call exit

; WAL hook: called by SQLite after every commit with the number of frames in the WAL.
wal_cb:
  mov [wal_frames], ecx
  xor eax, eax
  ret

; Checkpoint thread: its own connection; copies WAL pages back into the database file
; (and fsyncs it) in the background, so the event loop does not pay for that.
; Under constant writes a background checkpoint never quite reaches the end of the WAL, and
; SQLite only rewinds the WAL after a complete one, so flush() finishes the last few frames itself.
ckpt_thread:
  push rbx
  mov rdi, [db_path]
  lea rsi, [ckpt_db]
  mov edx, 0x8002
  xor ecx, ecx
  call sqlite3_open_v2
  test eax, eax
  jnz .out
  mov rdi, [ckpt_db]
  lea rsi, [sql_ckpt_pragmas]
  xor edx, edx
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_exec
.loop:
  mov edi, 50000              ; 50 ms
  call usleep
  cmp dword [wal_frames], CKPT_BG_FRAMES
  jb .loop
  mov rdi, [ckpt_db]
  xor esi, esi                ; main database
  xor edx, edx                ; SQLITE_CHECKPOINT_PASSIVE
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_wal_checkpoint_v2
  jmp .loop
.out:
  xor eax, eax
  pop rbx
  ret

; ---------------------------------------------------------------------------
; event loop

event_loop:
  push rbp
  mov rbp, rsp
  push rbx
  push r12
  push r13
  push r14
.wait:
  mov edi, [epfd]
  lea rsi, [events]
  mov edx, MAXEV
  mov r10d, -1
  SYSCALL SYS_epoll_wait
  test eax, eax
  jle .wait
  mov r12d, eax
  xor edi, edi
  call time
  mov [now], rax
  xor r13d, r13d
.ev:
  cmp r13d, r12d
  jae .done
  ; a full batch takes a while under load: take new connections every 128 events too,
  ; so the kernel's accept queue (somaxconn, 4096) doesn't overflow and drop SYNs
  test r13d, 127
  jnz .one
  test r13d, r13d
  jz .one
  cmp dword [accept_off], 0
  jne .one
  call do_accept
.one:
  lea eax, [r13d + r13d*2]
  lea rbx, [events]
  lea rbx, [rbx + rax*4]
  inc r13d
  mov r14d, [rbx]             ; event mask
  mov edi, [rbx+4]            ; fd
  cmp edi, [lfd]
  je .accept
  test r14d, EPOLLOUT
  jz .read
  call on_writable
  jmp .ev
.read:
  call on_readable
  jmp .ev
.accept:
  call do_accept
  jmp .ev
.done:
  call flush
  jmp .wait

do_accept:
  push rbx
  push r12
  push r13
  mov r12d, 1024              ; per call, so a connect storm can't starve requests
.l:
  mov edi, [lfd]
  lea rsi, [acc_addr]
  mov dword [acc_alen], 16
  lea rdx, [acc_alen]
  mov r10d, SOCK_NONBLOCK
  SYSCALL SYS_accept4
  test rax, rax
  js .err
  mov ebx, eax
  cmp ebx, MAXCONN
  jae .reject
  mov edi, ebx
  mov esi, 6                  ; IPPROTO_TCP
  mov edx, 1                  ; TCP_NODELAY
  lea r10, [one]
  mov r8d, 4
  SYSCALL SYS_setsockopt
  CONN rax, rbx
  xor ecx, ecx
  mov [rax], rcx
  mov [rax+8], rcx
  mov [rax+16], rcx
  mov [rax+24], rcx
  ; Keep-alive has two budgets. Past KA_LIMIT open connections in total, or KA_PER_IP from
  ; one client address, new connections are answered with "Connection: close" (one request
  ; each). The first keeps idle clients from running the fd table out. The second keeps one
  ; address (a NAT, a proxy, a load generator) under the ~28k local ports a Linux client has
  ; by default: a keep-alive connection pins a client port, while one the server closes first
  ; frees it at once.
  mov ecx, [acc_addr+4]       ; IPv4 address
  imul ecx, ecx, 0x9E3779B1
  shr ecx, 16                 ; bucket 0..65535
  mov [rax+C_IPB], ecx
  lea rdx, [ipcnt]
  lea rdx, [rdx + rcx*4]
  mov ecx, F_ALIVE
  cmp dword [nconn], KA_LIMIT
  jae .noka
  cmp dword [rdx], KA_PER_IP
  jb .ka
.noka:
  mov ecx, F_ALIVE | F_NOKA
.ka:
  inc dword [rdx]
  mov [rax+C_FLAGS], ecx
  inc dword [nconn]
  mov edi, EPOLL_CTL_ADD
  mov esi, ebx
  mov edx, EPOLLIN
  call ep_ctl
  dec r12d
  jnz .l
.ret:
  pop r13
  pop r12
  pop rbx
  ret
.err:
  cmp rax, -EMFILE
  je .full
  cmp rax, -ENFILE
  jne .ret
.full:
  ; no fd left: stop polling the listen socket (it would stay readable and spin the loop)
  ; until a connection closes. New clients wait in the kernel's accept queue meanwhile.
  mov dword [accept_off], 1
  mov edi, EPOLL_CTL_MOD
  mov esi, [lfd]
  xor edx, edx
  call ep_ctl
  jmp .ret
.reject:
  mov edi, ebx
  SYSCALL SYS_close
  jmp .l

; close_conn(edi = fd)
close_conn:
  push rbx
  push r12
  push r13
  mov ebx, edi
  CONN r12, rbx
  test dword [r12+C_FLAGS], F_ALIVE
  jz .ret
  mov edi, ebx
  SYSCALL SYS_close
  dec dword [nconn]
  mov eax, [r12+C_IPB]
  lea rcx, [ipcnt]
  dec dword [rcx + rax*4]
  cmp dword [accept_off], 0
  je .accepting
  mov dword [accept_off], 0   ; an fd is free again: resume accepting
  mov edi, EPOLL_CTL_MOD
  mov esi, [lfd]
  mov edx, EPOLLIN
  call ep_ctl
.accepting:
  mov rdi, [r12+C_INBUF]
  test rdi, rdi
  jz .noin
  call free
.noin:
  mov rdi, [r12+C_OUTBUF]
  test rdi, rdi
  jz .noout
  call free
.noout:
  xor eax, eax
  mov [r12], rax
  mov [r12+8], rax
  mov [r12+16], rax
  mov [r12+24], rax
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; stash(edi = fd, rsi = data, edx = len): keep a partial request for the next read
stash:
  push rbx
  push r12
  push r13
  push r14
  sub rsp, 8
  mov ebx, edi
  mov r12, rsi
  mov r13d, edx
  mov edi, edx
  call malloc
  mov r14, rax
  mov rdi, rax
  mov rsi, r12
  mov ecx, r13d
  rep movsb
  CONN rax, rbx
  mov [rax+C_INBUF], r14
  mov [rax+C_INLEN], r13d
  add rsp, 8
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; take_stash(r12 = conn) -> eax = bytes copied to the start of scratch
take_stash:
  push rbx
  mov rsi, [r12+C_INBUF]
  test rsi, rsi
  jz .none
  mov ebx, [r12+C_INLEN]
  lea rdi, [scratch]
  mov ecx, ebx
  rep movsb
  mov rdi, [r12+C_INBUF]
  call free
  mov qword [r12+C_INBUF], 0
  mov dword [r12+C_INLEN], 0
  mov eax, ebx
  pop rbx
  ret
.none:
  xor eax, eax
  pop rbx
  ret

; on_readable(edi = fd)
on_readable:
  push rbx
  push r12
  push r13
  mov ebx, edi
  CONN r12, rbx
  test dword [r12+C_FLAGS], F_ALIVE
  jz .ret
  cmp dword [r12+C_OUTLEN], 0
  jne .ret
  call take_stash
  mov r13d, eax
  mov edi, ebx
  lea rsi, [scratch]
  add rsi, r13
  mov edx, SCRATCH_SZ
  sub edx, r13d
  SYSCALL SYS_read
  test rax, rax
  jg .got
  cmp rax, -EAGAIN
  je .again
  cmp rax, -EINTR
  je .again
  mov edi, ebx
  call close_conn
  jmp .ret
.again:
  test r13d, r13d
  jz .ret
  mov edi, ebx
  lea rsi, [scratch]
  mov edx, r13d
  call stash
  jmp .ret
.got:
  add r13d, eax
  mov edi, ebx
  mov esi, r13d
  call process_data
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; on_writable(edi = fd): continue sending pending output
on_writable:
  push rbx
  push r12
  push r13
  mov ebx, edi
  CONN r12, rbx
  test dword [r12+C_FLAGS], F_ALIVE
  jz .ret
  mov ecx, [r12+C_OUTLEN]
  test ecx, ecx
  jz .asread
  mov edi, ebx
  mov rsi, [r12+C_OUTBUF]
  mov eax, [r12+C_OUTOFF]
  add rsi, rax
  mov edx, ecx
  sub edx, eax
  mov r10d, MSG_NOSIGNAL
  xor r8d, r8d
  xor r9d, r9d
  SYSCALL SYS_sendto
  cmp rax, -EAGAIN
  je .ret
  test rax, rax
  js .close
  add [r12+C_OUTOFF], eax
  mov eax, [r12+C_OUTOFF]
  cmp eax, [r12+C_OUTLEN]
  jb .ret
  mov rdi, [r12+C_OUTBUF]
  call free
  mov qword [r12+C_OUTBUF], 0
  mov qword [r12+C_OUTLEN], 0
  test dword [r12+C_FLAGS], F_CLOSE_AFTER
  jnz .close
  mov edi, EPOLL_CTL_MOD
  mov esi, ebx
  mov edx, EPOLLIN
  call ep_ctl
  call take_stash             ; requests that arrived while we were blocked
  test eax, eax
  jz .ret
  mov edi, ebx
  mov esi, eax
  call process_data
  jmp .ret
.asread:
  mov edi, ebx
  call on_readable
  jmp .ret
.close:
  mov edi, ebx
  call close_conn
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; store_pending(edi = fd, rsi = record, edx = bytes already sent)
store_pending:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov ebx, edi
  CONN r12, rbx
  mov r13d, [rsi+R_START]
  add r13d, edx               ; arena offset of the unsent part
  mov r14d, [rsi+R_LEN]
  sub r14d, edx               ; unsent length
  mov r15d, [r12+C_OUTLEN]    ; already pending
  mov rdi, [r12+C_OUTBUF]
  lea esi, [r15d + r14d]
  call realloc                ; realloc(NULL, n) == malloc(n)
  mov [r12+C_OUTBUF], rax
  lea rdi, [rax + r15]
  lea rsi, [arena]
  add rsi, r13
  mov ecx, r14d
  rep movsb
  add [r12+C_OUTLEN], r14d
  test r15d, r15d
  jnz .ret
  mov edi, EPOLL_CTL_MOD
  mov esi, ebx
  mov edx, EPOLLOUT
  call ep_ctl
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; flush: commit the batch transaction, then send every queued response
flush:
  push rbx
  push r12
  push r13
  push r14
  push r15
  xor r15d, r15d              ; commit failed?
  cmp dword [in_txn], 0
  je .send
  mov rdi, [st_commit]
  call sqlite3_step
  mov ebx, eax
  mov rdi, [st_commit]
  call sqlite3_reset
  mov dword [in_txn], 0
  cmp ebx, SQLITE_DONE
  je .send
  mov rdi, [db]
  lea rsi, [sql_rollback]
  xor edx, edx
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_exec
  mov r15d, 1
.send:
  xor r12d, r12d
.rl:
  cmp r12d, [nrec]
  jae .done
  mov eax, r12d
  shl rax, 4
  lea r13, [recs]
  add r13, rax
  inc r12d
  mov ebx, [r13+R_FD]
  CONN r14, rbx
  test dword [r14+C_FLAGS], F_ALIVE
  jz .rl
  test r15d, r15d
  jz .ok
  test dword [r13+R_FLAGS], RF_WRITE
  jz .ok
  mov edi, ebx                ; never acknowledge a write that did not commit
  call close_conn
  jmp .rl
.ok:
  xor edx, edx
  cmp dword [r14+C_OUTLEN], 0
  jne .pending
  mov edi, ebx
  lea rsi, [arena]
  mov eax, [r13+R_START]
  add rsi, rax
  mov edx, [r13+R_LEN]
  mov r10d, MSG_NOSIGNAL
  test dword [r13+R_FLAGS], RF_CLOSE
  jz .sflags
  ; the connection closes right after this send: hold the data back (MSG_MORE) so close()
  ; puts the FIN on the same segment. The client then always sees data and FIN together,
  ; closes second, and keeps no TIME_WAIT on its side (it would pin one of its ports for 60 s).
  mov r10d, MSG_NOSIGNAL | MSG_MORE
.sflags:
  xor r8d, r8d
  xor r9d, r9d
  SYSCALL SYS_sendto
  cmp rax, -EAGAIN
  je .zero
  test rax, rax
  js .close
  jmp .chk
.zero:
  xor eax, eax
.chk:
  cmp eax, [r13+R_LEN]
  je .sent
  mov edx, eax
.pending:
  mov edi, ebx
  mov rsi, r13
  call store_pending
  test dword [r13+R_FLAGS], RF_CLOSE
  jz .rl
  or dword [r14+C_FLAGS], F_CLOSE_AFTER
  jmp .rl
.sent:
  test dword [r13+R_FLAGS], RF_CLOSE
  jz .rl
.close:
  mov edi, ebx
  call close_conn
  jmp .rl
.done:
  mov qword [arena_used], 0
  mov dword [nrec], 0
  cmp dword [wal_frames], CKPT_MAIN_FRAMES
  jb .nockpt
  mov dword [wal_frames], 0
  mov rdi, [db]               ; nothing is writing now, so this checkpoint completes
  xor esi, esi                ; and the next commit starts the WAL from the beginning
  xor edx, edx                ; SQLITE_CHECKPOINT_PASSIVE
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_wal_checkpoint_v2
.nockpt:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; emit(ecx = status index, rdx = end of the body in bodybuf): queue one response
emit:
  push rbx
  push r12
  push r13
  lea r12, [bodybuf]
  mov r13, rdx
  sub r13, r12                ; body length
  lea rdi, [arena]
  add rdi, [arena_used]
  mov rbx, rdi                ; response start
  mov eax, ecx
  shl eax, 4
  lea rsi, [status_tab]
  mov rcx, [rsi+rax+8]
  mov rsi, [rsi+rax]
  rep movsb
  W_LIT `Content-Type: application/json\r\nContent-Length: `
  mov rax, r13
  call w_u64
  test dword [cur_flags], RF_CLOSE
  jz .ka
  W_LIT `\r\nConnection: close`
.ka:
  W_LIT `\r\n\r\n`
  mov rsi, r12
  mov rcx, r13
  rep movsb
  lea rax, [arena]
  mov rcx, rdi
  sub rcx, rax                ; new arena_used
  mov rdx, rbx
  sub rdx, rax                ; start offset
  mov [arena_used], rcx
  sub ecx, edx                ; response length
  mov r8d, [cur_fd]
  mov eax, [nrec]
  test eax, eax
  jz .new
  lea r9, [recs]
  lea r10d, [eax-1]
  shl r10, 4
  add r9, r10
  cmp [r9+R_FD], r8d
  jne .new
  mov r10d, [r9+R_START]
  add r10d, [r9+R_LEN]
  cmp r10d, edx
  jne .new
  add [r9+R_LEN], ecx         ; pipelined response right after the previous one
  jmp .flags
.new:
  lea r9, [recs]
  mov r10d, eax
  shl r10, 4
  add r9, r10
  mov [r9+R_FD], r8d
  mov [r9+R_START], edx
  mov [r9+R_LEN], ecx
  mov dword [r9+R_FLAGS], 0
  inc dword [nrec]
.flags:
  mov eax, [cur_flags]
  or [r9+R_FLAGS], eax
  pop r13
  pop r12
  pop rbx
  ret

; ---------------------------------------------------------------------------
; HTTP

; ci_eq: compare rcx bytes at rsi (any case) with the lowercase text at rdi. ZF=1 if equal.
; Clobbers al, rcx, rsi, rdi.
ci_eq:
  mov al, [rsi]
  or al, 0x20
  cmp al, [rdi]
  jne .ret
  inc rsi
  inc rdi
  dec rcx
  jnz ci_eq
.ret:
  ret

; process_data(edi = fd, esi = bytes in scratch): handle every complete request
process_data:
  push rbp
  mov rbp, rsp
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov ebx, edi
  mov [cur_fd], edi
  lea r12, [scratch]          ; r12 = start of the next request
  lea r13, [r12 + rsi]        ; r13 = end of data
.next:
  cmp r12, r13
  jae .ret
  ; find the end of the headers
  mov rdi, r12
.find:
  lea rax, [rdi+3]
  cmp rax, r13
  jae .incomplete
  cmp dword [rdi], 0x0A0D0A0D
  je .found
  inc rdi
  jmp .find
.found:
  lea r14, [rdi+4]            ; r14 = body start
  lea r15, [rdi+2]            ; r15 = end of the last header line (incl. CRLF)
  CONN rax, rbx
  xor ecx, ecx
  test dword [rax+C_FLAGS], F_NOKA
  jz .ka_ok
  mov ecx, RF_CLOSE
.ka_ok:
  mov [cur_flags], ecx
  xor eax, eax
  mov [req_blen], rax
  mov [req_auth], rax
  mov [req_alen], rax
  mov [req_method], eax
  ; request line
  mov rsi, r12
  cmp dword [rsi], 'GET '
  jne .notget
  mov dword [req_method], 1
  add rsi, 4
  jmp .path
.notget:
  cmp dword [rsi], 'POST'
  jne .othermeth
  cmp byte [rsi+4], ' '
  jne .othermeth
  mov dword [req_method], 2
  add rsi, 5
  jmp .path
.othermeth:
  cmp rsi, r15
  jae .bad
  mov al, [rsi]
  inc rsi
  cmp al, ' '
  jne .othermeth
.path:
  mov [req_path], rsi
  mov rdi, rsi
.pl:
  cmp rdi, r15
  jae .bad
  mov al, [rdi]
  cmp al, ' '
  je .pend
  cmp al, '?'
  je .pend
  cmp al, 13
  je .pend
  inc rdi
  jmp .pl
.pend:
  mov rax, rdi
  sub rax, rsi
  jz .bad
  mov [req_plen], rax
  ; skip the rest of the request line
.eol:
  cmp rdi, r15
  jae .bad
  cmp byte [rdi], 10
  je .hdrs0
  inc rdi
  jmp .eol
.hdrs0:
  lea rsi, [rdi+1]
.hdr:                         ; rsi = start of a header line
  cmp rsi, r15
  jae .hdone
  ; line end
  mov rdi, rsi
.le:
  cmp byte [rdi], 10
  je .lend
  inc rdi
  jmp .le
.lend:
  mov r8, rdi                 ; r8 = '\n'
  mov r9, rdi
  cmp r9, rsi
  je .nextline
  cmp byte [r9-1], 13
  jne .vstart
  dec r9                      ; r9 = end of the line content
.vstart:
  ; colon
  mov rdi, rsi
.colon:
  cmp rdi, r9
  jae .nextline
  cmp byte [rdi], ':'
  je .gotcolon
  inc rdi
  jmp .colon
.gotcolon:
  mov rcx, rdi
  sub rcx, rsi                ; name length
  lea r10, [rdi+1]            ; value start
.vs:
  cmp r10, r9
  jae .vtrim
  cmp byte [r10], ' '
  je .vs1
  cmp byte [r10], 9
  jne .vtrim
.vs1:
  inc r10
  jmp .vs
.vtrim:
  cmp r9, r10
  jbe .vdone
  cmp byte [r9-1], ' '
  je .vt1
  cmp byte [r9-1], 9
  jne .vdone
.vt1:
  dec r9
  jmp .vtrim
.vdone:                       ; name rsi/rcx, value r10..r9
  cmp rcx, 14
  je .maybe_cl
  cmp rcx, 13
  je .maybe_auth
  cmp rcx, 10
  je .maybe_conn
  jmp .nextline
.maybe_cl:
  push rsi
  lea rdi, [h_content_length]
  call ci_eq
  pop rsi
  jne .nextline
  xor eax, eax
  mov rdi, r10
  cmp rdi, r9
  jae .bad
.cl:
  cmp rdi, r9
  jae .clok
  movzx edx, byte [rdi]
  sub edx, '0'
  cmp edx, 9
  ja .bad
  imul rax, rax, 10
  add rax, rdx
  cmp rax, MAX_REQ
  ja .toobig
  inc rdi
  jmp .cl
.clok:
  mov [req_blen], rax
  jmp .nextline
.maybe_auth:
  push rsi
  lea rdi, [h_authorization]
  call ci_eq
  pop rsi
  jne .nextline
  mov [req_auth], r10
  mov rax, r9
  sub rax, r10
  mov [req_alen], rax
  jmp .nextline
.maybe_conn:
  push rsi
  lea rdi, [h_connection]
  call ci_eq
  pop rsi
  jne .nextline
  cmp r9, r10
  jbe .nextline
  mov al, [r10]
  or al, 0x20
  cmp al, 'c'                 ; "close"
  jne .nextline
  or dword [cur_flags], RF_CLOSE
.nextline:
  lea rsi, [r8+1]
  jmp .hdr
.hdone:
  ; is the whole body here?
  mov rax, r14
  add rax, [req_blen]
  cmp rax, r13
  ja .incomplete
  mov [req_body], r14
  push rax
  push rax
  call route
  pop rax
  pop rax
  mov r12, rax
  test dword [cur_flags], RF_CLOSE
  jnz .ret                    ; ignore anything after "Connection: close"
  ; keep the arena from overflowing
  mov rax, [arena_used]
  cmp rax, ARENA_SZ - ARENA_SLACK
  jae .flushnow
  cmp dword [nrec], MAXREC - 2
  jb .cont
.flushnow:
  call flush
.cont:
  CONN rax, rbx
  test dword [rax+C_FLAGS], F_ALIVE
  jz .ret
  cmp dword [rax+C_OUTLEN], 0
  je .next
  ; the socket is full: keep the rest until it drains
  mov rdx, r13
  sub rdx, r12
  jz .ret
  mov edi, ebx
  mov rsi, r12
  call stash
  jmp .ret
.incomplete:
  mov rdx, r13
  sub rdx, r12
  cmp rdx, MAX_REQ
  jae .toobig
  mov edi, ebx
  mov rsi, r12
  call stash
  jmp .ret
.toobig:
.bad:
  mov dword [cur_flags], RF_CLOSE
  REPLY ST_400, '{"error":"bad request"}'
.ret:
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  pop rbp
  ret

section .rodata
h_content_length: db 'content-length'
h_authorization:  db 'authorization'
h_connection:     db 'connection'
section .text

; parse_id(rdi = text, rdx = length) -> rax = id, or 0 if it is not a positive integer
parse_id:
  xor eax, eax
  test rdx, rdx
  jz .bad
  cmp rdx, 18
  ja .bad
.l:
  movzx ecx, byte [rdi]
  sub ecx, '0'
  cmp ecx, 9
  ja .bad
  imul rax, rax, 10
  add rax, rcx
  inc rdi
  dec rdx
  jnz .l
  ret
.bad:
  xor eax, eax
  ret

; route: dispatch the parsed request in req_*
route:
  push rbx
  push r12
  push r13
  mov rsi, [req_path]
  mov rcx, [req_plen]
  mov eax, [req_method]
  cmp eax, 1
  je .get
  cmp eax, 2
  je .post
  jmp .notfound
.get:
  cmp rcx, 5
  jne .g2
  cmp dword [rsi], '/fee'
  jne .notfound
  cmp byte [rsi+4], 'd'
  jne .notfound
  call h_feed
  jmp .ret
.g2:
  cmp rcx, 7
  jb .notfound
  jne .g3
  cmp dword [rsi], '/hea'
  jne .g3
  cmp dword [rsi+3], 'alth'
  jne .g3
  call h_health
  jmp .ret
.g3:
  cmp dword [rsi], '/pos'
  jne .notfound
  cmp word [rsi+4], 'ts'
  jne .notfound
  cmp byte [rsi+6], '/'
  jne .notfound
  lea rdi, [rsi+7]
  lea rdx, [rcx-7]
  call has_slash
  jz .notfound
  lea rdi, [rsi+7]
  lea rdx, [rcx-7]
  call parse_id
  test rax, rax
  jz .badid
  mov rdi, rax
  call h_get_post
  jmp .ret
.post:
  cmp rcx, 6
  jne .p2
  cmp dword [rsi], '/pos'
  jne .notfound
  cmp word [rsi+4], 'ts'
  jne .notfound
  call h_create
  jmp .ret
.p2:
  cmp rcx, 12
  jb .notfound
  cmp dword [rsi], '/pos'
  jne .notfound
  cmp word [rsi+4], 'ts'
  jne .notfound
  cmp byte [rsi+6], '/'
  jne .notfound
  cmp dword [rsi+rcx-4], 'like'
  jne .notfound
  cmp byte [rsi+rcx-5], '/'
  jne .notfound
  lea rdi, [rsi+7]
  lea rdx, [rcx-12]
  call has_slash
  jz .notfound
  mov r12, rsi
  mov r13, rcx
  call do_auth                ; auth is checked before the id
  test eax, eax
  jnz .ret
  lea rdi, [r12+7]
  lea rdx, [r13-12]
  call parse_id
  test rax, rax
  jz .badid
  mov rdi, rax
  call h_like
  jmp .ret
.badid:
  REPLY ST_400, '{"error":"invalid post id"}'
  jmp .ret
.notfound:
  REPLY ST_404, '{"error":"not found"}'
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; has_slash(rdi, rdx): ZF=1 if the range contains '/'. Clobbers rdi, rdx.
has_slash:
  test rdx, rdx
  jz .no
.l:
  cmp byte [rdi], '/'
  je .yes
  inc rdi
  dec rdx
  jnz .l
.no:
  or edx, 1                   ; ZF=0
  ret
.yes:
  ret

; ---------------------------------------------------------------------------
; auth

; do_auth -> eax = 0 if the token is valid (jwt_uid/jwt_name set), else the 401 is queued
do_auth:
  push rbx
  call auth_check
  test eax, eax
  jz .ret
  mov ebx, eax
  cmp ebx, 1
  jne .a2
  REPLY ST_401, '{"error":"missing bearer token"}'
  jmp .fail
.a2:
  cmp ebx, 2
  jne .a3
  REPLY ST_401, '{"error":"invalid or expired token"}'
  jmp .fail
.a3:
  REPLY ST_401, '{"error":"invalid token payload"}'
.fail:
  mov eax, ebx
.ret:
  pop rbx
  ret

; auth_check -> eax: 0 ok, 1 missing bearer, 2 invalid or expired, 3 invalid payload
auth_check:
  push rbp
  mov rbp, rsp
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 72                 ; [rsp] signature, [rsp+32] computed mac
  mov rsi, [req_auth]
  mov rdx, [req_alen]
  test rsi, rsi
  jz .missing
  cmp rdx, 7
  jb .missing
  cmp dword [rsi], 'Bear'
  jne .missing
  cmp dword [rsi+3], 'rer '
  jne .missing
  lea r12, [rsi+7]            ; token
  lea r13, [rsi+rdx]          ; token end
  ; the two dots
  mov rdi, r12
.d1:
  cmp rdi, r13
  jae .invalid
  cmp byte [rdi], '.'
  je .gd1
  inc rdi
  jmp .d1
.gd1:
  mov r14, rdi
  inc rdi
.d2:
  cmp rdi, r13
  jae .invalid
  cmp byte [rdi], '.'
  je .gd2
  inc rdi
  jmp .d2
.gd2:
  mov r15, rdi
  ; signature: exactly 32 bytes
  lea rsi, [r15+1]
  mov rdx, r13
  sub rdx, rsi
  cmp rdx, 43
  jne .invalid
  mov rdi, rsp
  call b64d
  cmp rax, 32
  jne .invalid
  mov rsi, r12
  mov rdx, r15
  sub rdx, r12
  lea rdi, [rsp+32]
  call hmac_sha256
  mov rax, [rsp]
  xor rax, [rsp+32]
  mov rcx, [rsp+8]
  xor rcx, [rsp+40]
  or rax, rcx
  mov rcx, [rsp+16]
  xor rcx, [rsp+48]
  or rax, rcx
  mov rcx, [rsp+24]
  xor rcx, [rsp+56]
  or rax, rcx
  jnz .invalid
  ; header: {"alg":"HS256",...}
  mov rsi, r12
  mov rdx, r14
  sub rdx, r12
  cmp rdx, 1024
  ja .invalid
  lea rdi, [hdrbuf]
  call b64d
  test rax, rax
  js .invalid
  lea rsi, [hdrbuf]
  lea rdx, [hdrbuf+rax]
  lea rdi, [keytab_hdr]
  call obj_scan
  jc .invalid
  test eax, eax
  jz .invalid
  mov rsi, [HDR_ALG]
  test rsi, rsi
  jz .invalid
  cmp byte [rsi], '"'
  jne .invalid
  lea rdi, [subbuf]
  call json_string
  lea rax, [subbuf]
  sub rdi, rax
  cmp rdi, 5
  jne .invalid
  cmp dword [subbuf], 'HS25'
  jne .invalid
  cmp byte [subbuf+4], '6'
  jne .invalid
  ; payload
  lea rsi, [r14+1]
  mov rdx, r15
  sub rdx, rsi
  cmp rdx, 4096
  ja .invalid
  lea rdi, [plbuf]
  call b64d
  test rax, rax
  js .invalid
  lea rsi, [plbuf]
  lea rdx, [plbuf+rax]
  mov rbx, rdx                ; payload end
  lea rdi, [keytab_pl]
  call obj_scan
  jc .invalid
  test eax, eax
  jz .invalid
  ; exp (optional): a number in the future
  mov rsi, [PL_EXP]
  test rsi, rsi
  jz .sub
  movzx eax, byte [rsi]
  cmp al, '-'
  je .invalid
  sub eax, '0'
  cmp eax, 9
  ja .invalid
  xor eax, eax
  xor ecx, ecx
.ed:
  cmp rsi, rbx
  jae .edone
  movzx edx, byte [rsi]
  sub edx, '0'
  cmp edx, 9
  ja .edone
  inc ecx
  cmp ecx, 18
  ja .sub                     ; absurdly far in the future
  imul rax, rax, 10
  add rax, rdx
  inc rsi
  jmp .ed
.edone:
  cmp rax, [now]
  jle .invalid
.sub:
  mov rsi, [PL_SUB]
  test rsi, rsi
  jz .payload
  cmp byte [rsi], '"'
  jne .payload
  mov rdx, rbx
  lea rdi, [subbuf]
  call json_string
  lea rdx, [subbuf]
  sub rdi, rdx
  mov rdx, rdi
  lea rdi, [subbuf]
  call parse_id
  test rax, rax
  jz .payload
  mov [jwt_uid], rax
  mov rsi, [PL_NAME]
  test rsi, rsi
  jz .payload
  cmp byte [rsi], '"'
  jne .payload
  mov rdx, rbx
  lea rdi, [namebuf]
  call json_string
  lea rax, [namebuf]
  mov [jwt_name], rax
  sub rdi, rax
  mov [jwt_nlen], rdi
  xor eax, eax
  jmp .ret
.missing:
  mov eax, 1
  jmp .ret
.invalid:
  mov eax, 2
  jmp .ret
.payload:
  mov eax, 3
.ret:
  add rsp, 72
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  pop rbp
  ret

; ---------------------------------------------------------------------------
; handlers (each queues exactly one response)

; write_post(rdi = statement on a row, rsi = out) -> rax = out end
; Columns: id, body, created_at, author, like_count
write_post:
  push rbx
  push r12
  push r13
  mov r12, rdi
  mov rdi, rsi
  W_LIT '{"id":'
  mov rbx, rdi
  mov rdi, r12
  xor esi, esi
  call sqlite3_column_int64
  mov rdi, rbx
  call w_u64
  W_LIT ',"body":'
  mov rbx, rdi
  mov esi, 1
  call .text
  W_LIT ',"created_at":'
  mov rbx, rdi
  mov esi, 2
  call .text
  W_LIT ',"author":'
  mov rbx, rdi
  mov esi, 3
  call .text
  W_LIT ',"like_count":'
  mov rbx, rdi
  mov rdi, r12
  mov esi, 4
  call sqlite3_column_int64
  mov rdi, rbx
  call w_u64
  mov byte [rdi], '}'
  lea rax, [rdi+1]
  pop r13
  pop r12
  pop rbx
  ret
.text:                        ; column esi as a JSON string at rbx -> rdi
  push r14                    ; (keeps the stack aligned for the C calls)
  mov r14d, esi
  mov rdi, r12
  call sqlite3_column_text
  mov r13, rax
  mov rdi, r12
  mov esi, r14d
  call sqlite3_column_bytes
  mov edx, eax
  mov rsi, r13
  mov rdi, rbx
  call w_jstr
  pop r14
  ret

h_feed:
  push rbx
  push r12
  push r13
  mov r12, [st_feed]
  lea rdi, [bodybuf]
  W_LIT '{"posts":['
  mov rbx, rdi
  xor r13d, r13d
.row:
  mov rdi, r12
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .end
  test r13d, r13d
  jz .first
  mov byte [rbx], ','
  inc rbx
.first:
  inc r13d
  mov rdi, r12
  mov rsi, rbx
  call write_post
  mov rbx, rax
  jmp .row
.end:
  mov r13d, eax
  mov rdi, r12
  call sqlite3_reset
  cmp r13d, SQLITE_DONE
  jne .err
  mov rdi, rbx
  W_LIT ']}'
  mov rdx, rdi
  mov ecx, ST_200
  call emit
  jmp .ret
.err:
  REPLY ST_500, '{"error":"internal server error"}'
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; h_get_post(rdi = id)
h_get_post:
  push rbx
  push r12
  push r13
  mov r12, [st_post]
  mov rdx, rdi
  mov rdi, r12
  mov esi, 1
  call sqlite3_bind_int64
  mov rdi, r12
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .norow
  lea rdi, [bodybuf]
  W_LIT '{"post":'
  mov rsi, rdi
  mov rdi, r12
  call write_post
  mov byte [rax], '}'
  lea rbx, [rax+1]
  mov rdi, r12
  call sqlite3_reset
  mov rdx, rbx
  mov ecx, ST_200
  call emit
  jmp .ret
.norow:
  mov ebx, eax
  mov rdi, r12
  call sqlite3_reset
  cmp ebx, SQLITE_DONE
  jne .err
  REPLY ST_404, '{"error":"post not found"}'
  jmp .ret
.err:
  REPLY ST_500, '{"error":"internal server error"}'
.ret:
  pop r13
  pop r12
  pop rbx
  ret

h_health:
  push rbx
  push r12
  push r13
  mov r12, [st_one]
  mov rdi, r12
  call sqlite3_step
  mov ebx, eax
  mov rdi, r12
  call sqlite3_reset
  cmp ebx, SQLITE_ROW
  jne .bad
  lea rdi, [bodybuf]
  W_LIT '{"status":"ok","db":"ok","uptime_s":'
  mov rax, [now]
  sub rax, [start_time]
  call w_u64
  mov byte [rdi], '}'
  lea rdx, [rdi+1]
  mov ecx, ST_200
  call emit
  jmp .ret
.bad:
  mov rdi, [db]
  call sqlite3_errmsg
  mov r13, rax
  mov rdi, rax
  call strlen
  mov rbx, rax
  lea rdi, [bodybuf]
  W_LIT '{"status":"degraded","db":"unreachable","error":'
  mov rsi, r13
  mov rdx, rbx
  call w_jstr
  mov byte [rdi], '}'
  lea rdx, [rdi+1]
  mov ecx, ST_503
  call emit
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; txn_begin -> eax = 0 when the batch transaction is open
txn_begin:
  push rbx
  xor eax, eax
  cmp dword [in_txn], 0
  jne .ret
  mov rdi, [st_begin]
  call sqlite3_step
  mov ebx, eax
  mov rdi, [st_begin]
  call sqlite3_reset
  mov eax, 1
  cmp ebx, SQLITE_DONE
  jne .ret
  mov dword [in_txn], 1
  xor eax, eax
.ret:
  pop rbx
  ret

h_create:
  push rbx
  push r12
  push r13
  push r14
  push r15
  call do_auth
  test eax, eax
  jnz .ret
  mov rsi, [req_body]
  mov rdx, [req_blen]
  add rdx, rsi
  mov r15, rdx                ; body end
  lea rdi, [keytab_body]
  call obj_scan
  jc .malformed
  test eax, eax
  jz .required
  mov rsi, [BODY_VAL]
  test rsi, rsi
  jz .required
  cmp byte [rsi], '"'
  jne .required
  mov rdx, r15
  lea rdi, [strbuf]
  call json_string
  lea rsi, [strbuf]
  call trim
  cmp rsi, rdi
  jae .required
  mov r12, rsi
  mov r13, rdi
  sub r13, rsi                ; trimmed length in bytes
  ; length in characters (code points)
  xor eax, eax
  xor ecx, ecx
.cp:
  movzx edx, byte [rsi+rcx]
  and edx, 0xC0
  cmp edx, 0x80
  je .cont
  inc eax
.cont:
  inc rcx
  cmp rcx, r13
  jb .cp
  cmp eax, 500
  ja .toolong
  call txn_begin
  test eax, eax
  jnz .err
  or dword [cur_flags], RF_WRITE
  mov r14, [st_ins]
  mov rdi, r14
  mov esi, 1
  mov rdx, [jwt_uid]
  call sqlite3_bind_int64
  mov rdi, r14
  mov esi, 2
  mov rdx, r12
  mov ecx, r13d
  xor r8d, r8d                ; SQLITE_STATIC
  call sqlite3_bind_text
  mov rdi, r14
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .stepfail
  lea rdi, [bodybuf]
  W_LIT '{"post":{"id":'
  mov rbx, rdi
  mov rdi, r14
  xor esi, esi
  call sqlite3_column_int64
  mov rdi, rbx
  call w_u64
  W_LIT ',"body":'
  mov rsi, r12
  mov rdx, r13
  call w_jstr
  W_LIT ',"created_at":'
  mov rbx, rdi
  mov rdi, r14
  mov esi, 1
  call sqlite3_column_text
  mov r15, rax
  mov rdi, r14
  mov esi, 1
  call sqlite3_column_bytes
  mov edx, eax
  mov rsi, r15
  mov rdi, rbx
  call w_jstr
  W_LIT ',"author":'
  mov rsi, [jwt_name]
  mov rdx, [jwt_nlen]
  call w_jstr
  W_LIT ',"like_count":0}}'
  mov rbx, rdi
  mov rdi, r14
  call sqlite3_reset
  mov rdx, rbx
  mov ecx, ST_201
  call emit
  jmp .ret
.stepfail:
  mov rdi, r14
  call sqlite3_reset
.err:
  REPLY ST_500, '{"error":"internal server error"}'
  jmp .ret
.malformed:
  REPLY ST_400, '{"error":"malformed JSON body"}'
  jmp .ret
.required:
  REPLY ST_400, '{"error":"body is required"}'
  jmp .ret
.toolong:
  REPLY ST_400, '{"error":"body must be at most 500 characters"}'
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; h_like(rdi = post id)
h_like:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  call txn_begin
  test eax, eax
  jnz .err
  or dword [cur_flags], RF_WRITE
  mov r12, [st_like]
  mov rdi, r12
  mov esi, 1
  mov rdx, [jwt_uid]
  call sqlite3_bind_int64
  mov rdi, r12
  mov esi, 2
  mov rdx, rbx
  call sqlite3_bind_int64
  mov rdi, r12
  call sqlite3_step
  mov r13d, eax
  mov rdi, r12
  call sqlite3_reset
  cmp r13d, SQLITE_DONE
  jne .err
  mov rdi, [db]
  call sqlite3_changes
  test eax, eax
  jz .exists
  lea rdi, [bodybuf]
  W_LIT '{"liked":true,"already_liked":false,"post_id":'
  mov rax, rbx
  call w_u64
  mov byte [rdi], '}'
  lea rdx, [rdi+1]
  mov ecx, ST_201
  call emit
  jmp .ret
.exists:
  mov r12, [st_exists]
  mov rdi, r12
  mov esi, 1
  mov rdx, rbx
  call sqlite3_bind_int64
  mov rdi, r12
  call sqlite3_step
  mov r13d, eax
  mov rdi, r12
  call sqlite3_reset
  cmp r13d, SQLITE_ROW
  jne .missing
  lea rdi, [bodybuf]
  W_LIT '{"liked":true,"already_liked":true,"post_id":'
  mov rax, rbx
  call w_u64
  mov byte [rdi], '}'
  lea rdx, [rdi+1]
  mov ecx, ST_200
  call emit
  jmp .ret
.missing:
  cmp r13d, SQLITE_DONE
  jne .err
  REPLY ST_404, '{"error":"post not found"}'
  jmp .ret
.err:
  REPLY ST_500, '{"error":"internal server error"}'
.ret:
  pop r13
  pop r12
  pop rbx
  ret

section .note.GNU-stack noalloc noexec nowrite progbits
