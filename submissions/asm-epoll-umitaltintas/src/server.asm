; The $12 server challenge in x86-64 assembly (NASM, Linux).
;
; One thread, io_uring for all socket I/O, one SQLite connection.
; Each pass over the completion queues is processed like this:
;   1. handle the data of every completed receive: parse and handle the requests in it
;   2. writes (POST /posts, likes) run inside one transaction opened by the first write
;   3. every response of the batch is built into one arena
;   4. COMMIT, then send all responses (so nobody sees a write before it is committed)
; SQLite is the only C code (built from the pinned amalgamation); libc is linked because SQLite needs it.

default rel
bits 64

%include "defs.inc"

extern getenv, malloc, realloc, free, time, exit, strlen, atoi, usleep, clock_gettime
extern sqlite3_initialize, sqlite3_open_v2, sqlite3_exec, sqlite3_prepare_v3, sqlite3_step
extern sqlite3_reset, sqlite3_bind_int64, sqlite3_bind_text, sqlite3_column_int64
extern sqlite3_column_text, sqlite3_column_bytes, sqlite3_changes, sqlite3_errmsg
extern sqlite3_wal_checkpoint_v2, sqlite3_wal_hook

global main

section .bss
alignb 64
conns:      resb MAXCONN << CONN_SHIFT
dq_buf:     resb DQ_CAP * 16       ; completions set aside while sending
wlist:      resd MAXCONN           ; connections whose socket became writable
recs:       resb MAXREC * 16
scratch:    resb SCRATCH_SZ + 64
bodybuf:    resb BODY_SZ
strbuf:     resb SCRATCH_SZ       ; decoded POST body
hdrbuf:     resb 1024             ; decoded JWT header
plbuf:      resb 4096             ; decoded JWT payload
subbuf:     resb 4096
namebuf:    resb 4096             ; decoded username
arena:      resb ARENA_SZ
fbuf:       resb 131072           ; feed: each post's JSON up to its like count
fcnt:       resd FEED_WIN         ; feed: like counts by post id - lowest id
alignb 8
fid:        resq 32
foff:       resd 32
fend:       resd 32

alignb 8
db:         resq 1
st_feed:    resq 1
st_lrange:  resq 1
st_cnt:     resq 1
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
lfd:        resd 1
cur_fd:     resd 1                ; connection id of the request being handled
cur_flags:  resd 1                ; RF_* for the response being built
nconn:      resd 1                ; open client connections
last_all:   resq 1                ; when every ring was last entered (µs)
dq_head:    resd 1
dq_tail:    resd 1
wcount:     resd 1
sends_out:  resd 1                ; sends submitted whose result hasn't been seen yet
acc_ring:   resd 1                ; ring that accepts new connections
acc_armed:  resd 1
acc_seq:    resd 1
acc_ud:     resq 1                ; user_data of the armed accept
in_txn:     resd 1
st_dqfull:  resd 1                ; problem counters, printed every 10 s when not all zero
st_flushbound: resd 1
st_recverr: resd 1
st_nobufs:  resd 1                ; receives that found no free buffer
st_lasterr: resd 1
st_cqover:  resd 1
st_switch:  resd 1
st_enfile:  resd 1
st_next:    resq 1
st_t0:      resq 1                ; start of the current pass (µs)
st_maxpass: resq 1                ; longest pass in this 10 s window (µs)
st_maxckpt: resq 1                ; longest checkpoint in this window (µs)
st_ckpts:   resd 1
st_ts:      resq 2
statbuf:    resb 256
fslow:      resd 1                ; feed: count likes post by post
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

sql_pragmas: db 'PRAGMA locking_mode=EXCLUSIVE;PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA mmap_size=0;'
             db 'PRAGMA cache_size=-16384;PRAGMA temp_store=MEMORY;PRAGMA busy_timeout=5000;'
             db 'PRAGMA wal_autocheckpoint=0;PRAGMA journal_size_limit=67108864;', 0
sql_feed:    db 'SELECT p.id,p.body,p.created_at,u.username '
             db 'FROM posts p JOIN users u ON u.id=p.user_id '
             db 'ORDER BY p.created_at DESC,p.id DESC LIMIT 20', 0
sql_lrange:  db 'SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2', 0
sql_cnt:     db 'SELECT count(*) FROM likes WHERE post_id=?1', 0
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
%include "uring.inc"
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
  call uring_init
  call ring_create            ; ring 0
  mov dword [acc_ring], 0
  call arm_accept

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
  lea rdi, [sql_lrange]
  call prep
  mov [st_lrange], rax
  lea rdi, [sql_cnt]
  call prep
  mov [st_cnt], rax
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
  mov ebx, 100
.bind:
  mov edi, [lfd]
  lea rsi, [sockaddr]
  mov edx, 16
  SYSCALL SYS_bind
  test eax, eax
  jz .bound
  ; a previous instance's rings can hold the port for a few seconds after it exits, while the
  ; kernel closes their sockets: retry for up to 50 s
  dec ebx
  jz .err
  mov edi, 500000
  call usleep
  jmp .bind
.bound:
  mov edi, [lfd]
  mov esi, 6                  ; IPPROTO_TCP
  mov edx, 1                  ; TCP_NODELAY (accepted sockets inherit it)
  lea r10, [one]
  mov r8d, 4
  SYSCALL SYS_setsockopt
  mov edi, [lfd]
  mov esi, 65535
  SYSCALL SYS_listen
  test eax, eax
  jnz .err
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

; conn: CONN dest, id (64-bit register holding a connection id) -> dest = &conns[id]
%macro CONN 2
  mov %1, %2
  shl %1, CONN_SHIFT
  lea r11, [conns]
  add %1, r11
%endmacro

; mk_ud(edi = op, esi = connection id) -> rax = user_data: op | ring << 8 | gen << 16 | slot << 32
; Clobbers rcx, rdx, r11.
mk_ud:
  mov eax, esi
  shl rax, CONN_SHIFT
  lea r11, [conns]
  movzx edx, word [r11+rax+C_GEN]
  shl edx, 16
  mov eax, esi
  and eax, 0xFFFF
  shl rax, 32
  or rax, rdx
  mov ecx, esi
  shr ecx, 16
  shl ecx, 8
  or rax, rcx
  or rax, rdi
  ret

; ud_conn(rdi = user_data) -> eax = connection id, ZF=1 if it still names a live connection
; (same generation). Clobbers rcx, rdx, r11.
ud_conn:
  mov rax, rdi
  shr rax, 32                 ; slot
  mov ecx, edi
  shr ecx, 8
  and ecx, 0xFF               ; ring
  shl ecx, 16
  or eax, ecx                 ; id
  mov ecx, eax
  shl rcx, CONN_SHIFT
  lea r11, [conns]
  add rcx, r11
  test dword [rcx+C_FLAGS], F_ALIVE
  jz .dead
  movzx edx, word [rcx+C_GEN]
  mov ecx, edi
  shr ecx, 16
  cmp cx, dx
  ret
.dead:
  or ecx, 1                   ; ZF=0
  ret

; dq_push(rax = user_data, edx = res, ecx = flags): queue a completion for the event loop
dq_push:
  mov r8d, [dq_tail]
  mov r9d, r8d
  sub r9d, [dq_head]
  cmp r9d, DQ_CAP
  jb .room
  inc dword [st_dqfull]       ; never expected (DQ_CAP > all completion queues together)
  ret
.room:
  and r8d, DQ_CAP - 1
  shl r8, 4
  lea r9, [dq_buf]
  add r8, r9
  mov [r8], rax
  mov [r8+8], edx
  mov [r8+12], ecx
  inc dword [dq_tail]
  ret

; next_event -> rax = user_data, edx = res, ecx = flags; CF=1 when there is nothing to do.
; Completions set aside while sending (dq) come first, so per-connection order is kept.
next_event:
  mov eax, [dq_head]
  cmp eax, [dq_tail]
  je .empty
  and eax, DQ_CAP - 1
  shl rax, 4
  lea r8, [dq_buf]
  add r8, rax
  mov rax, [r8]
  mov edx, [r8+8]
  mov ecx, [r8+12]
  inc dword [dq_head]
  clc
  ret
.empty:
  xor eax, eax                ; start over at the front: only the deepest the queue ever got
  mov [dq_head], eax          ; is touched, not all DQ_CAP entries
  mov [dq_tail], eax
.rings:
  xor r9d, r9d
.rl:
  cmp r9d, [nrings]
  jae .none
  RING r8, r9
  inc r9d
  mov rsi, [r8+RG_CQHEAD]
  mov r10d, [rsi]
  mov rdi, [r8+RG_CQTAIL]
  cmp r10d, [rdi]
  je .rl
  and r10d, [r8+RG_CQMASK]
  shl r10, 4
  add r10, [r8+RG_CQES]
  mov rax, [r10]
  mov edx, [r10+8]
  mov ecx, [r10+12]
  inc dword [rsi]             ; consume (after reading the entry)
  clc
  ret
.none:
  stc
  ret

; dispatch(rax = user_data, edx = res, ecx = flags)
dispatch:
  mov rdi, rax
  mov esi, edx
  mov edx, ecx
  movzx eax, dil
  cmp eax, OP_RECV
  je on_recv
  cmp eax, OP_ACCEPT
  je on_accept_cqe
  cmp eax, OP_SEND
  je on_send
  cmp eax, OP_POLL
  je on_poll
  cmp eax, OP_RESUME
  je on_resume
  cmp eax, OP_RPOLL
  je on_rpoll
  ret                         ; cancel/close results need nothing

; on_rpoll(rdi = user_data, edx = flags): ring 0's poll on another ring. Its completions are read
; by the loop anyway; only re-arm the poll if the kernel ended it.
on_rpoll:
  test edx, CQE_F_MORE
  jnz .ret
  shr edi, 8
  and edi, 0xFF
  jmp arm_rpoll
.ret:
  ret

; ---------------------------------------------------------------------------
; accepting

; pick_accept_ring -> eax = ring to accept on, or -1 if every ring is (nearly) full
pick_accept_ring:
  xor eax, eax
.l:
  cmp eax, [nrings]
  jae .new
  RING rcx, rax
  mov edx, [slots]
  sub edx, ACC_LOW
  cmp [rcx+RG_NOPEN], edx
  jb .ret
  inc eax
  jmp .l
.new:
  cmp eax, MAXRINGS
  jae .full
  call ring_create
.ret:
  ret
.full:
  mov eax, -1
  ret

; arm_accept: start accepting on ring [acc_ring]: ACC_K single-shot accepts in flight, each
; straight into the ring's fixed-file table. (Multishot accept was seen to stop delivering on
; Linux 6.8 under load; a single-shot accept is re-armed by the loop as each one completes.)
arm_accept:
  push rbx
  mov ecx, [acc_seq]
  movzx ecx, cx
  shl ecx, 16
  mov edx, [acc_ring]
  shl edx, 8
  or ecx, edx
  or ecx, OP_ACCEPT
  mov [acc_ud], rcx
  mov dword [acc_armed], 1
  mov ebx, ACC_K
.l:
  call arm_one_accept
  dec ebx
  jnz .l
  pop rbx
  ret

; arm_one_accept: one more single-shot accept with user_data [acc_ud] on ring [acc_ring]
arm_one_accept:
  mov edi, [acc_ring]
  RING rdi, rdi
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_ACCEPT
  mov ecx, [lfd]
  mov [rax+SQE_FD], ecx
  mov dword [rax+SQE_FILE_INDEX], FILE_INDEX_ALLOC
  mov rcx, [acc_ud]
  mov [rax+SQE_USER_DATA], rcx
  ret

; accept_elsewhere: cancel the accepts on [acc_ring] and continue on a ring with room
accept_elsewhere:
  push rbx
  inc dword [st_switch]
  mov edi, [acc_ring]
  RING rdi, rdi
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_ASYNC_CANCEL
  mov dword [rax+SQE_FD], -1
  mov dword [rax+SQE_OPFLAGS], CANCEL_ALL
  mov rcx, [acc_ud]
  mov [rax+SQE_ADDR], rcx     ; every request with this user_data
  mov qword [rax+SQE_USER_DATA], OP_CANCEL
  mov dword [acc_armed], 0
  call rearm_accept
  pop rbx
  ret

; rearm_accept: start accepting on the first ring with room, if not accepting already
rearm_accept:
  cmp dword [acc_armed], 0
  jne .ret
  call pick_accept_ring
  test eax, eax
  js .ret
  mov [acc_ring], eax
  inc dword [acc_seq]
  call arm_accept
.ret:
  ret

; on_accept_cqe(rdi = user_data, esi = res, edx = flags)
on_accept_cqe:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12d, esi
  test esi, esi
  js .done
  mov edi, ebx
  shr edi, 8
  and edi, 0xFF
  call on_accept              ; (may move accepting to another ring)
.done:
  cmp rbx, [acc_ud]           ; an accept of the current set: replace it
  jne .ret
  cmp dword [acc_armed], 0
  je .ret
  cmp r12d, -ENFILE
  je .full
  cmp r12d, -ECANCELED
  je .ret
  call arm_one_accept
  jmp .ret
.full:
  inc dword [st_enfile]
  call accept_elsewhere
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; on_accept(edi = ring, esi = slot)
on_accept:
  push rbx
  push r12
  push r13
  mov r13d, edi
  mov ebx, edi
  shl ebx, 16
  or ebx, esi                 ; connection id
  CONN r12, rbx
  xor eax, eax
  mov [r12+C_INBUF], rax
  mov [r12+C_INLEN], eax
  mov dword [r12+C_FLAGS], F_ALIVE
  mov [r12+C_OUTBUF], rax
  mov [r12+C_OUTLEN], rax
  RING rdi, r13
  inc dword [rdi+RG_NOPEN]
  inc dword [nconn]
  mov esi, ebx
  call arm_recv
  ; keep a reserve of free slots for accepts already in flight: move on before the table fills
  cmp r13d, [acc_ring]
  jne .ret
  cmp dword [acc_armed], 0
  je .ret
  RING rdi, r13
  mov eax, [slots]
  sub eax, ACC_RESERVE
  cmp [rdi+RG_NOPEN], eax
  jb .ret
  call accept_elsewhere
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; arm_recv(esi = connection id): multishot receive into the ring's provided buffers (re-armed when
; a completion comes without IORING_CQE_F_MORE)
arm_recv:
  push rbx
  mov ebx, esi
  mov edi, OP_RECV
  call mk_ud
  push rax
  mov ecx, ebx
  shr ecx, 16
  RING rdi, rcx
  call get_sqe
  pop rcx
  mov [rax+SQE_USER_DATA], rcx
  mov byte [rax+SQE_OPCODE], OPC_RECV
  mov byte [rax+SQE_FLAGS], IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT
  mov word [rax+SQE_IOPRIO], RECV_MULTISHOT
  movzx ecx, bx
  mov [rax+SQE_FD], ecx
  pop rbx
  ret

; close_conn(edi = connection id)
close_conn:
  push rbx
  push r12
  push r13
  mov ebx, edi
  CONN r12, rbx
  test dword [r12+C_FLAGS], F_ALIVE
  jz .ret
  inc word [r12+C_GEN]        ; completions still on their way are now stale
  mov dword [r12+C_FLAGS], 0
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
  mov [r12+C_INBUF], rax
  mov [r12+C_INLEN], eax
  mov [r12+C_OUTBUF], rax
  mov [r12+C_OUTLEN], rax
  dec dword [nconn]
  mov ecx, ebx
  shr ecx, 16
  RING r13, rcx
  dec dword [r13+RG_NOPEN]
  ; shut the socket down, then free the slot. The shutdown sends the FIN now (on Linux 6.8 the
  ; close only releases the socket once every older request on the ring has finished, which can
  ; take minutes) and ends whatever is armed on the socket: the multishot receive sees EOF, a
  ; POLLOUT poll sees POLLHUP. Their completions find a new generation and are dropped. (An
  ; ASYNC_CANCEL by fd would scan every armed poll on the ring, accepts included, per close.)
  mov rdi, r13
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_SHUTDOWN
  mov byte [rax+SQE_FLAGS], IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS
  movzx ecx, bx
  mov [rax+SQE_FD], ecx
  mov dword [rax+SQE_LEN], 2  ; SHUT_RDWR
  mov qword [rax+SQE_USER_DATA], OP_CLOSE
  mov rdi, r13
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_CLOSE
  mov byte [rax+SQE_FLAGS], IOSQE_CQE_SKIP_SUCCESS
  movzx ecx, bx
  inc ecx
  mov [rax+SQE_FILE_INDEX], ecx
  mov qword [rax+SQE_USER_DATA], OP_CLOSE
  call rearm_accept
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; ---------------------------------------------------------------------------
; receiving

; on_recv(rdi = user_data, esi = res, edx = flags)
on_recv:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r15, rdi
  mov r12d, esi               ; res
  mov r13d, edx               ; flags
  mov r14d, -1                ; buffer id, if any
  test r13d, CQE_F_BUFFER
  jz .nobuf
  mov r14d, r13d
  shr r14d, 16
.nobuf:
  call ud_conn
  mov ebx, eax                ; connection id
  jnz .done                   ; stale
  test r12d, r12d
  jle .noread
  mov ecx, r15d
  shr ecx, 8
  and ecx, 0xFF
  RING rdi, rcx
  mov esi, r14d
  shl rsi, BUF_SHIFT
  add rsi, [rdi+RG_BUFBASE]
  mov edx, r12d
  mov edi, ebx
  call on_data
  test r13d, CQE_F_MORE
  jnz .done
.rearm:
  mov rdi, r15
  call ud_conn                ; still open after handling the data?
  jnz .done
  mov esi, ebx
  call arm_recv
  jmp .done
.noread:
  cmp r12d, -ENOBUFS
  jne .notnobufs
  inc dword [st_nobufs]
  jmp .rearm                  ; every buffer was in use: try again
.notnobufs:
  test r12d, r12d
  jz .eof
  inc dword [st_recverr]
  mov [st_lasterr], r12d
.eof:
  mov edi, ebx                ; end of stream or an error
  call close_conn
.done:
  test r14d, r14d
  js .ret
  mov ecx, r15d
  shr ecx, 8
  and ecx, 0xFF
  RING rdi, rcx
  mov esi, r14d
  call put_buf
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; on_data(edi = connection id, rsi = data, edx = length)
on_data:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov ebx, edi
  CONN r12, rbx
  mov r13, rsi
  mov r14d, edx
  test dword [r12+C_FLAGS], F_CLOSING
  jnz .ret                    ; a "Connection: close" response is on its way: ignore the rest
  cmp dword [r12+C_OUTLEN], 0
  jne .hold
  mov rdi, [r12+C_INBUF]
  test rdi, rdi
  jnz .join
  mov edi, ebx
  mov rsi, r13
  mov edx, r14d
  call process_data
  jmp .ret
.join:                        ; the start of this request came earlier: stash + new data
  mov r15d, [r12+C_INLEN]
  lea eax, [r15d + r14d]
  cmp eax, SCRATCH_SZ
  ja .close
  call take_stash
  lea rdi, [scratch]
  add rdi, r15
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  mov edi, ebx
  lea rsi, [scratch]
  lea edx, [r15d + r14d]
  call process_data
  jmp .ret
.hold:                        ; output is backed up: keep the input until it drains
  mov r15d, [r12+C_INLEN]
  lea esi, [r15d + r14d]
  cmp esi, MAX_REQ * 2
  ja .close
  mov rdi, [r12+C_INBUF]
  call realloc
  mov [r12+C_INBUF], rax
  lea rdi, [rax + r15]
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  add [r12+C_INLEN], r14d
  jmp .ret
.close:
  mov edi, ebx
  call close_conn
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; on_resume(rdi = user_data): output drained; handle the input that waited for it
on_resume:
  push rbx
  push r12
  push r13
  call ud_conn
  jnz .ret
  mov ebx, eax
  CONN r12, rbx
  cmp dword [r12+C_OUTLEN], 0
  jne .ret
  call take_stash
  test eax, eax
  jz .ret
  mov edi, ebx
  lea rsi, [scratch]
  mov edx, eax
  call process_data
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; stash(edi = connection id, rsi = data, edx = len): keep a partial request for the next read
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

; ---------------------------------------------------------------------------
; sending

; queue_send(edi = connection id, rsi = data, edx = length): a non-blocking send. It completes
; while io_uring_enter submits it (MSG_DONTWAIT), so the data only has to live until then.
queue_send:
  push rbx
  push r12
  push r13
  mov ebx, edi
  mov r12, rsi
  mov r13d, edx
  CONN rax, rbx
  or dword [rax+C_FLAGS], F_SENDING
  mov [rax+C_SPTR], r12
  mov [rax+C_SLEN], r13d
  mov edi, OP_SEND
  mov esi, ebx
  call mk_ud
  push rax
  mov ecx, ebx
  shr ecx, 16
  RING rdi, rcx
  call get_sqe
  pop rcx
  mov [rax+SQE_USER_DATA], rcx
  mov byte [rax+SQE_OPCODE], OPC_SEND
  mov byte [rax+SQE_FLAGS], IOSQE_FIXED_FILE
  movzx ecx, bx
  mov [rax+SQE_FD], ecx
  mov [rax+SQE_ADDR], r12
  mov [rax+SQE_LEN], r13d
  mov dword [rax+SQE_OPFLAGS], MSG_DONTWAIT | MSG_NOSIGNAL
  inc dword [sends_out]
  ; the last response of a closing connection: the FIN follows in the same segment, before the
  ; client is done reading. (If the client closed first, its port would sit in TIME_WAIT for 60 s.)
  ; Up to 16 KB always fits in a fresh socket's send buffer, so the send is never partial here.
  CONN rcx, rbx
  test dword [rcx+C_FLAGS], F_CLOSE_AFTER
  jz .done
  cmp r13d, 16384
  ja .done
  or byte [rax+SQE_FLAGS], IOSQE_IO_LINK
  or dword [rax+SQE_OPFLAGS], MSG_MORE
  mov ecx, ebx
  shr ecx, 16
  RING rdi, rcx
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_SHUTDOWN
  mov byte [rax+SQE_FLAGS], IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS
  movzx ecx, bx
  mov [rax+SQE_FD], ecx
  mov dword [rax+SQE_LEN], 1  ; SHUT_WR
  mov qword [rax+SQE_USER_DATA], OP_CLOSE
.done:
  pop r13
  pop r12
  pop rbx
  ret

; send_out(edi = connection id): send the connection's pending output buffer
send_out:
  CONN rax, rdi
  or dword [rax+C_FLAGS], F_SEND_OUT
  mov rsi, [rax+C_OUTBUF]
  mov ecx, [rax+C_OUTOFF]
  add rsi, rcx
  mov edx, [rax+C_OUTLEN]
  sub edx, ecx
  jmp queue_send

; append_out(edi = connection id, rsi = data, edx = length): add to the pending output
append_out:
  push rbx
  push r12
  push r13
  push r14
  sub rsp, 8
  mov ebx, edi
  CONN r12, rbx
  mov r13, rsi
  mov r14d, edx
  mov rdi, [r12+C_OUTBUF]
  mov esi, [r12+C_OUTLEN]
  add esi, r14d
  call realloc
  mov [r12+C_OUTBUF], rax
  mov edi, [r12+C_OUTLEN]
  add rdi, rax
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  add [r12+C_OUTLEN], r14d
  add rsp, 8
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; arm_pollout(edi = connection id): get a completion when the socket can take more data
arm_pollout:
  push rbx
  mov ebx, edi
  CONN rax, rbx
  test dword [rax+C_FLAGS], F_POLLING
  jnz .ret
  or dword [rax+C_FLAGS], F_POLLING
  mov edi, OP_POLL
  mov esi, ebx
  call mk_ud
  push rax
  mov ecx, ebx
  shr ecx, 16
  RING rdi, rcx
  call get_sqe
  pop rcx
  mov [rax+SQE_USER_DATA], rcx
  mov byte [rax+SQE_OPCODE], OPC_POLL_ADD
  mov byte [rax+SQE_FLAGS], IOSQE_FIXED_FILE
  movzx ecx, bx
  mov [rax+SQE_FD], ecx
  mov dword [rax+SQE_OPFLAGS], POLLOUT
.ret:
  pop rbx
  ret

; on_poll(rdi = user_data, esi = res): the socket is writable again; send at the next flush
on_poll:
  push rbx
  mov ebx, esi
  call ud_conn
  jnz .ret
  CONN rcx, rax
  and dword [rcx+C_FLAGS], ~F_POLLING
  test ebx, ebx
  js .err
  test dword [rcx+C_FLAGS], F_WRITABLE
  jnz .ret
  or dword [rcx+C_FLAGS], F_WRITABLE
  mov edx, [wcount]
  lea r8, [wlist]
  mov [r8+rdx*4], eax
  inc dword [wcount]
  jmp .ret
.err:
  mov edi, eax
  call close_conn
.ret:
  pop rbx
  ret

; on_send(rdi = user_data, esi = res): result of a send submitted by flush
on_send:
  push rbx
  push r12
  push r13
  push r14
  push r15
  dec dword [sends_out]
  mov r13d, esi
  call ud_conn
  jnz .ret
  mov ebx, eax
  CONN r12, rbx
  and dword [r12+C_FLAGS], ~F_SENDING
  test r13d, r13d
  jns .sent
  cmp r13d, -EAGAIN
  jne .close
  xor r13d, r13d
.sent:
  test dword [r12+C_FLAGS], F_SEND_OUT
  jz .arena
  add [r12+C_OUTOFF], r13d
  mov eax, [r12+C_OUTOFF]
  cmp eax, [r12+C_OUTLEN]
  jb .wait
  and dword [r12+C_FLAGS], ~F_SEND_OUT
  mov rdi, [r12+C_OUTBUF]
  call free
  mov qword [r12+C_OUTBUF], 0
  mov qword [r12+C_OUTLEN], 0
  jmp .drained
.arena:
  cmp r13d, [r12+C_SLEN]
  jae .full
  ; short send: the rest goes in front of anything already pending
  mov r14d, [r12+C_SLEN]
  sub r14d, r13d              ; unsent bytes
  mov r15d, [r12+C_OUTLEN]
  sub r15d, [r12+C_OUTOFF]    ; pending bytes
  lea edi, [r14d + r15d]
  call malloc
  mov rdi, rax
  mov rsi, [r12+C_SPTR]
  add rsi, r13
  mov ecx, r14d
  rep movsb
  mov rsi, [r12+C_OUTBUF]
  test rsi, rsi
  jz .nopend
  mov ecx, [r12+C_OUTOFF]
  add rsi, rcx
  mov ecx, r15d
  rep movsb
.nopend:
  push rax
  push rax
  mov rdi, [r12+C_OUTBUF]
  call free
  pop rax
  pop rax
  mov [r12+C_OUTBUF], rax
  lea eax, [r14d + r15d]
  mov [r12+C_OUTLEN], eax
  mov dword [r12+C_OUTOFF], 0
.wait:
  mov edi, ebx
  call arm_pollout
  jmp .ret
.full:
  cmp dword [r12+C_OUTLEN], 0
  je .drained
  mov edi, ebx                ; later responses for this connection were held back: send them
  call send_out
  jmp .ret
.drained:
  test dword [r12+C_FLAGS], F_CLOSE_AFTER
  jnz .close
  cmp qword [r12+C_INBUF], 0
  je .ret
  mov edi, OP_RESUME          ; input waited for this: handle it in the event loop
  mov esi, ebx
  call mk_ud
  xor edx, edx
  xor ecx, ecx
  call dq_push
  jmp .ret
.close:
  mov edi, ebx
  call close_conn
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; reap: take every completion off the rings. Send results are handled now (their data may live
; in the arena); everything else is queued for the event loop, in order.
reap:
  push rbx
  push r12
  push r13
  xor r12d, r12d
.ring:
  cmp r12d, [nrings]
  jae .ret
  RING rbx, r12
.l:
  mov rsi, [rbx+RG_CQHEAD]
  mov r10d, [rsi]
  mov rdi, [rbx+RG_CQTAIL]
  cmp r10d, [rdi]
  je .next
  and r10d, [rbx+RG_CQMASK]
  shl r10, 4
  add r10, [rbx+RG_CQES]
  mov rax, [r10]
  mov edx, [r10+8]
  mov ecx, [r10+12]
  inc dword [rsi]
  cmp al, OP_SEND
  je .send
  call dq_push
  jmp .l
.send:
  mov rdi, rax
  mov esi, edx
  call on_send
  jmp .l
.next:
  inc r12d
  jmp .ring
.ret:
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
  jae .writable
  mov eax, r12d
  shl rax, 4
  lea r13, [recs]
  add r13, rax
  inc r12d
  mov ebx, [r13+R_FD]
  CONN r14, rbx
  test dword [r14+C_FLAGS], F_ALIVE
  jz .rl
  movzx eax, word [r14+C_GEN]
  mov ecx, [r13+R_FLAGS]
  shr ecx, 16
  cmp eax, ecx
  jne .rl                     ; closed (and the slot reused) since the request
  test r15d, r15d
  jz .ok
  test dword [r13+R_FLAGS], RF_WRITE
  jz .ok
  mov edi, ebx                ; never acknowledge a write that did not commit
  call close_conn
  jmp .rl
.ok:
  test dword [r13+R_FLAGS], RF_CLOSE
  jz .noclose
  or dword [r14+C_FLAGS], F_CLOSE_AFTER
.noclose:
  lea rsi, [arena]
  mov eax, [r13+R_START]
  add rsi, rax
  mov edx, [r13+R_LEN]
  mov edi, ebx
  test dword [r14+C_FLAGS], F_SENDING
  jnz .append
  cmp dword [r14+C_OUTLEN], 0
  jne .append
  call queue_send
  jmp .rl
.append:
  call append_out
  jmp .rl
.writable:                    ; connections whose socket drained since the last flush
  xor r12d, r12d
.wl:
  cmp r12d, [wcount]
  jae .submit
  lea rax, [wlist]
  mov ebx, [rax+r12*4]
  inc r12d
  CONN r14, rbx
  test dword [r14+C_FLAGS], F_WRITABLE
  jz .wl
  and dword [r14+C_FLAGS], ~F_WRITABLE
  test dword [r14+C_FLAGS], F_SENDING | F_POLLING
  jnz .wl
  mov eax, [r14+C_OUTLEN]
  cmp eax, [r14+C_OUTOFF]
  jbe .wl
  mov edi, ebx
  call send_out
  jmp .wl
.submit:
  mov dword [wcount], 0
  mov r12d, 64                ; sends finish inside io_uring_enter; this is only a safety bound
.sl:
  xor esi, esi
  call enter_all
  call reap
  cmp dword [sends_out], 0
  je .sent
  dec r12d
  jnz .sl
  inc dword [st_flushbound]   ; never expected: a send did not complete inside io_uring_enter
.sent:
  mov qword [arena_used], 0
  mov dword [nrec], 0
  cmp dword [wal_frames], CKPT_MAIN_FRAMES
  jb .nockpt
  mov dword [wal_frames], 0
  call now_us
  mov rbx, rax
  mov rdi, [db]               ; nothing is writing now, so this checkpoint completes
  xor esi, esi                ; and the next commit starts the WAL from the beginning
  xor edx, edx                ; SQLITE_CHECKPOINT_PASSIVE
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_wal_checkpoint_v2
  call now_us
  sub rax, rbx
  inc dword [st_ckpts]
  cmp rax, [st_maxckpt]
  jbe .nockpt
  mov [st_maxckpt], rax
.nockpt:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; now_us -> rax = monotonic clock in microseconds
now_us:
  sub rsp, 8
  mov edi, 1                  ; CLOCK_MONOTONIC
  lea rsi, [st_ts]
  call clock_gettime
  mov rax, [st_ts]
  imul rax, rax, 1000000
  mov rcx, rax
  mov rax, [st_ts+8]
  xor edx, edx
  mov r8d, 1000
  div r8
  add rax, rcx
  add rsp, 8
  ret

; print_stats: one line on stderr if anything unexpected happened (counters are cumulative)
print_stats:
  push rbx
  push r12
  sub rsp, 8
  ; completions the kernel had to drop because a completion queue overflowed
  xor ecx, ecx
  xor ebx, ebx
.ov:
  cmp ecx, [nrings]
  jae .ovd
  RING rdx, rcx
  mov rax, [rdx+RG_CQOVF]
  add ebx, [rax]
  inc ecx
  jmp .ov
.ovd:
  mov [st_cqover], ebx
  mov eax, [st_dqfull]
  or eax, [st_flushbound]
  or eax, [st_recverr]
  or eax, [st_nobufs]
  or eax, ebx
  jnz .print
  cmp qword [st_maxpass], 50000
  jb .reset
.print:
  lea rdi, [statbuf]
  W_LIT 'stats: recv errors '
  mov eax, [st_recverr]
  call w_u64
  W_LIT ' (last '
  mov eax, [st_lasterr]
  neg eax
  call w_u64
  W_LIT '), no buffer '
  mov eax, [st_nobufs]
  call w_u64
  W_LIT ', cq overflow '
  mov eax, [st_cqover]
  call w_u64
  W_LIT ', dq full '
  mov eax, [st_dqfull]
  call w_u64
  W_LIT ', flush bound '
  mov eax, [st_flushbound]
  call w_u64
  W_LIT ', connections '
  mov eax, [nconn]
  call w_u64
  W_LIT ', rings '
  mov eax, [nrings]
  call w_u64
  W_LIT ', accept moves '
  mov eax, [st_switch]
  call w_u64
  W_LIT ' (table full '
  mov eax, [st_enfile]
  call w_u64
  W_LIT ')' 
  W_LIT ', longest pass us '
  mov rax, [st_maxpass]
  call w_u64
  W_LIT ', longest checkpoint us '
  mov rax, [st_maxckpt]
  call w_u64
  W_LIT ', checkpoints '
  mov eax, [st_ckpts]
  call w_u64
  mov byte [rdi], 10
  inc rdi
  lea rsi, [statbuf]
  mov rdx, rdi
  sub rdx, rsi
  mov edi, 2
  SYSCALL SYS_write
.reset:
  xor eax, eax
  mov [st_maxpass], rax
  mov [st_maxckpt], rax
  mov [st_ckpts], eax
.ret:
  add rsp, 8
  pop r12
  pop rbx
  ret

; ---------------------------------------------------------------------------
; event loop

event_loop:
  push rbp
  mov rbp, rsp
  push rbx
  push r12
.pass:
  call now_us
  mov [st_t0], rax
  xor esi, esi
  cmp dword [nrings], 1
  jbe .enter
  mov rcx, rax
  sub rcx, [last_all]
  cmp rcx, 1000
  jb .enter
  mov [last_all], rax
  mov esi, 1                  ; every ring, once a millisecond
.enter:
  call enter_all              ; submit queued sqes; post completions waiting in the kernel
  xor edi, edi
  call time
  mov [now], rax
  cmp rax, [st_next]
  jb .ev0
  add rax, 10
  mov [st_next], rax
  call print_stats
.ev0:
  xor r12d, r12d              ; events handled in this pass
.ev:
  cmp r12d, PASS_MAX          ; a long pass delays every response in it: flush now and then
  jae .end
  call next_event
  jc .end
  inc r12d
  call dispatch
  jmp .ev
.end:
  cmp dword [nrec], 0
  jne .flush
  cmp dword [in_txn], 0
  jne .flush
  cmp dword [wcount], 0
  je .noflush
.flush:
  call flush
.noflush:
  ; more to do right away? (completions set aside, or already posted on a ring)
  mov eax, [dq_head]
  cmp eax, [dq_tail]
  jne .pass
  xor ebx, ebx
.cq:
  cmp ebx, [nrings]
  jae .wait
  RING rdi, rbx
  inc ebx
  mov rax, [rdi+RG_CQHEAD]
  mov eax, [rax]
  mov rcx, [rdi+RG_CQTAIL]
  cmp eax, [rcx]
  jne .pass
  jmp .cq
.wait:
  call now_us
  sub rax, [st_t0]
  cmp rax, [st_maxpass]
  jbe .wt
  mov [st_maxpass], rax
.wt:
  test r12d, r12d
  jz .idle
  ; busy: let a batch build up (bigger group commits, fewer trips into the kernel)
  mov esi, BATCH_N
  lea rdx, [ts_batch]
  call ring_wait
  jmp .pass
.idle:
  ; nothing happened: sleep, unless a ring still has work for the kernel
  xor ebx, ebx
.chk:
  cmp ebx, [nrings]
  jae .sleep
  RING rdi, rbx
  inc ebx
  call ring_needs_enter
  jnz .pass
  jmp .chk
.sleep:
  mov esi, 1
  xor edx, edx
  cmp dword [nrings], 1
  jbe .forever
  lea rdx, [ts_idle]
.forever:
  call ring_wait
  jmp .pass

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
  mov eax, [cur_fd]
  shl rax, CONN_SHIFT
  lea rsi, [conns]
  or dword [rsi+rax+C_FLAGS], F_CLOSING
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
  mov r10d, r8d
  shl r10, CONN_SHIFT
  lea r11, [conns]
  movzx r10d, word [r11+r10+C_GEN]
  shl r10d, 16                ; the connection's generation: a reused slot gets nothing of this
  mov [r9+R_FLAGS], r10d
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

; process_data(edi = connection id, rsi = data, edx = length): handle every complete request
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
  mov r12, rsi                ; r12 = start of the next request
  lea r13, [rsi + rdx]        ; r13 = end of data
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
  mov dword [cur_flags], 0
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
  call txn_begin              ; every request of the pass shares one transaction (one set of locks)
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
  ; exp (required): a number in the future
  mov rsi, [PL_EXP]
  test rsi, rsi
  jz .invalid
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

; col_jstr(rdi = statement, esi = column, rdx = out) -> rax = end of the JSON string written
col_jstr:
  push rbx
  push r12
  push r13
  push r14
  sub rsp, 8
  mov rbx, rdi
  mov r12d, esi
  mov r13, rdx
  call sqlite3_column_text
  mov r14, rax
  mov rdi, rbx
  mov esi, r12d
  call sqlite3_column_bytes
  mov edx, eax
  mov rsi, r14
  mov rdi, r13
  call w_jstr
  mov rax, rdi
  add rsp, 8
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; count_likes(rdi = post id) -> rax = like count, or -1 on an error
count_likes:
  push rbx
  push r12
  sub rsp, 8
  mov rdx, rdi
  mov r12, [st_cnt]
  mov rdi, r12
  mov esi, 1
  call sqlite3_bind_int64
  mov rdi, r12
  call sqlite3_step
  mov rbx, -1
  cmp eax, SQLITE_ROW
  jne .reset
  mov rdi, r12
  xor esi, esi
  call sqlite3_column_int64
  mov rbx, rax
.reset:
  mov rdi, r12
  call sqlite3_reset
  mov rax, rbx
  add rsp, 8
  pop r12
  pop rbx
  ret

; GET /feed. The 20 newest posts come from one index scan joined to users. Their like counts
; come from one range scan of likes_post_id_idx over [lowest id, highest id] (the newest posts
; have nearly consecutive ids), counted here, instead of one count(*) lookup per post.
h_feed:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, [st_feed]
  lea rbx, [fbuf]             ; write position
  xor r13d, r13d              ; posts so far
.row:
  mov rdi, r12
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .rows_done
  cmp r13d, 32
  jae .row
  lea rax, [fbuf]
  mov rcx, rbx
  sub rcx, rax
  lea rdx, [foff]
  mov [rdx+r13*4], ecx
  mov rdi, r12
  xor esi, esi
  call sqlite3_column_int64
  lea rdx, [fid]
  mov [rdx+r13*8], rax
  mov rdi, rbx
  W_LIT '{"id":'
  call w_u64
  W_LIT ',"body":'
  mov rdx, rdi
  mov rdi, r12
  mov esi, 1
  call col_jstr
  mov rdi, rax
  W_LIT ',"created_at":'
  mov rdx, rdi
  mov rdi, r12
  mov esi, 2
  call col_jstr
  mov rdi, rax
  W_LIT ',"author":'
  mov rdx, rdi
  mov rdi, r12
  mov esi, 3
  call col_jstr
  mov rbx, rax
  lea rax, [fbuf]
  mov rcx, rbx
  sub rcx, rax
  lea rdx, [fend]
  mov [rdx+r13*4], ecx
  inc r13d
  jmp .row
.rows_done:
  mov r14d, eax
  mov rdi, r12
  call sqlite3_reset
  cmp r14d, SQLITE_DONE
  jne .err
  ; lowest and highest id
  mov dword [fslow], 1
  test r13d, r13d
  jz .assemble
  lea rdx, [fid]
  mov r14, [rdx]              ; min
  mov r15, r14                ; max
  xor ecx, ecx
.mm:
  mov rax, [rdx+rcx*8]
  cmp rax, r14
  cmovb r14, rax
  cmp rax, r15
  cmova r15, rax
  inc ecx
  cmp ecx, r13d
  jb .mm
  mov rax, r15
  sub rax, r14
  cmp rax, FEED_WIN
  jae .assemble               ; ids too far apart: count each post on its own
  lea rdi, [fcnt]
  lea ecx, [eax+1]
  xor eax, eax
  rep stosd
  mov r12, [st_lrange]
  mov rdi, r12
  mov esi, 1
  mov rdx, r14
  call sqlite3_bind_int64
  mov rdi, r12
  mov esi, 2
  mov rdx, r15
  call sqlite3_bind_int64
.like:
  mov rdi, r12
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .likes_done
  mov rdi, r12
  xor esi, esi
  call sqlite3_column_int64
  sub rax, r14
  cmp rax, FEED_WIN
  jae .like
  lea rdx, [fcnt]
  inc dword [rdx+rax*4]
  jmp .like
.likes_done:
  mov ebx, eax
  mov rdi, r12
  call sqlite3_reset
  cmp ebx, SQLITE_DONE
  jne .err
  mov dword [fslow], 0
.assemble:
  lea rdi, [bodybuf]
  W_LIT '{"posts":['
  mov rbx, rdi
  xor r12d, r12d
.post:
  cmp r12d, r13d
  jae .close
  test r12d, r12d
  jz .first
  mov byte [rbx], ','
  inc rbx
.first:
  lea rdx, [foff]
  mov esi, [rdx+r12*4]
  lea rdx, [fend]
  mov ecx, [rdx+r12*4]
  sub ecx, esi
  lea rax, [fbuf]
  add rsi, rax
  mov rdi, rbx
  rep movsb
  mov rbx, rdi
  lea rdx, [fid]
  mov rdi, [rdx+r12*8]
  cmp dword [fslow], 0
  jne .slow
  sub rdi, r14
  lea rdx, [fcnt]
  mov eax, [rdx+rdi*4]
  jmp .count
.slow:
  call count_likes
  test rax, rax
  js .err
.count:
  mov rdi, rbx
  W_LIT ',"like_count":'
  call w_u64
  mov byte [rdi], '}'
  lea rbx, [rdi+1]
  inc r12d
  jmp .post
.close:
  mov rdi, rbx
  W_LIT ']}'
  mov rdx, rdi
  mov ecx, ST_200
  call emit
  jmp .ret
.err:
  REPLY ST_500, '{"error":"internal server error"}'
.ret:
  pop r15
  pop r14
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
