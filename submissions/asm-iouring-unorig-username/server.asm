; Feed API server in x86-64 assembly (NASM, System V ABI, Linux).
; A port of submissions/c-iouring-unorig-username/server.c: one io_uring event loop on raw io_uring_setup/
; enter/register syscalls (no liburing), HTTP/1.1, routing, strict JSON, base64url, SHA-256/HMAC, the JWT
; checks, JSON output, SQLite record decoding and the feed plan are all here. shim.c is the C glue that
; reaches SQLite's B-tree layer (static inside the amalgamation): open, cursors, record bytes, a page view,
; and the checkpoint thread. Every SQL statement is prepared and stepped from here.

bits 64
default rel

%ifndef COMBO
%define COMBO 1            ; 1: send pool of 2048 (with shim.c's WAL write coalescing, the same switch)
%endif
%define MAXREQ       65536          ; per-request header+body cap
%define BATCH_OUT    (4*1024*1024)  ; response bytes held per batch until its commit
%define MAXSEG       65536          ; responses per batch
%define MAXRINGS     32
%define SQ_ENTRIES   4096
%define CQ_ENTRIES   16384
%ifndef NBUF                 ; overridable only to exercise these paths in tests
%define NBUF 2048  ; provided receive buffers per ring: every one gets touched, so they cost RAM
%endif
%define BSZ          1024
%define BSZ_SHIFT    10
%define NLISTEN      8              ; SO_REUSEPORT listeners: each has its own accept queue of somaxconn
%define ACCEPTS      64             ; per listener, rearmed once per loop pass, so this caps accepts per pass
%ifndef ANSWERS                 ; overridable only to exercise these paths in tests
%define ANSWERS 1024  ; requests a pass answers ...
%endif
%define READS        4096           ; ... and single-shot reads it issues after a stall
%define WAIT_N       128            ; completions a busy pass waits for, shared across the rings
%define FEED_N       20
%define FEED_SPAN    512            ; ids this close together share one walk of the likes index
%ifndef WAL_START                 ; overridable only to exercise these paths in tests
%define WAL_START 8192  ; log frames before a checkpoint pass is asked for
%endif
%define WAL_MAX      (4*WAL_START)
%define POST_WAIT_NS 250000000
%define POOL_CAP     (8192 - 24)
%if COMBO
%define POOL_MAX     2048           ; >= ANSWERS: a full pass never mallocs a send buffer
%else
%define POOL_MAX     512
%endif

; connection (64 bytes), per ring slot
%define C_IN      0      ; q: partial request, or input held while a send is in flight
%define C_Q       8      ; q: responses queued behind the send in flight
%define C_INLEN   16     ; d
%define C_INCAP   20     ; d
%define C_GEN     24     ; d
%define C_OPEN    28     ; b
%define C_CLOSING 29     ; b
%define C_SENDING 30     ; b
%define C_EOF     31     ; b: the client finished sending
%define C_OUT     32     ; q: the send in flight
%define C_SENT100 40     ; b: "100 Continue" sent for the request being read
%define C_QUEUED  41     ; b: on the read queue
%define C_READY   42     ; b: on the ready list
; send buffer: {who q, len d, off d, cap d, pad, bytes}
%define S_WHO 0
%define S_LEN 8
%define S_OFF 12
%define S_CAP 16
%define S_D   24
; ring (128 bytes)
%define R_FD      0      ; d
%define R_N       4      ; d: open connections
%define R_PEND    8      ; d: accepts in flight
%define R_TAIL    12     ; d: our SQ tail
%define R_SQHEAD  16     ; q: pointers into the mapped rings
%define R_SQTAIL  24
%define R_SQMASK  32     ; d
%define R_SQENT   36     ; d
%define R_SQES    40
%define R_CQHEAD  48
%define R_CQTAIL  56
%define R_CQMASK  64     ; d
%define R_CQES    72
%define R_SQFLAGS 80
%define R_CONNS   88
%define R_BR      96     ; provided buffer ring
%define R_BUFS    104
%define R_READS   112    ; d: single-shot reads in flight
%define R_EFD     116    ; d: what io_uring_enter is given: the registered ring index, or the fd
%define R_EFL     120    ; d: ENTER_REGISTERED_RING or 0

; io_uring ABI (include/uapi/linux/io_uring.h)
%define SYS_io_uring_setup 425
%define SYS_io_uring_enter 426
%define SYS_io_uring_register 427
%define SYS_eventfd2 290
%define P_SQ 40                     ; io_uring_params.sq_off
%define P_CQ 80                     ; io_uring_params.cq_off
%define SETUP_FLAGS (8 | 128 | 4096 | 8192)  ; CQSIZE | SUBMIT_ALL | SINGLE_ISSUER | DEFER_TASKRUN
%define REG_RING_FDS 20
%define ENTER_REGISTERED_RING 16
%define REG_FILES2 13
%define REG_PBUF_RING 22
%define OP_SYNC_FILE_RANGE 8
%define OP_ACCEPT 13
%define OP_ASYNC_CANCEL 14
%define OP_CLOSE 19
%define OP_READ 22
%define OP_SEND 26
%define OP_RECV 27
%define SQE_FIXED_FILE 1
%define SQE_IO_HARDLINK 8
%define SQE_ASYNC 16
%define SQE_BUFFER_SELECT 32
%define RECV_MULTISHOT 2
%define FILE_INDEX_ALLOC 0xffffffff
%define CQE_F_BUFFER 1
%define CQE_F_MORE 2
%define SQ_CQ_OVERFLOW 2
%define ENTER_GETEVENTS 1
%define ENTER_EXT_ARG 8
%define MSG_NOSIGNAL 0x4000
%define MSG_DONTWAIT 0x40
; completion tags (low 3 bits of user_data; 0 is a send, tagged by its buffer address)
%define T_ACCEPT 1
%define T_RECV 2
%define T_NOP 3
%define T_READ 4
%define T_WAL 5

; sqlite constants
%define SQLITE_ROW    100
%define SQLITE_DONE   101
%define SQLITE_OPEN_RW 2
%define SQLITE_PREP_PERSISTENT 1

; syscalls
%define SYS_write 1
%define SYS_close 3
%define SYS_mmap 9
%define SYS_madvise 28
%define SYS_socket 41
%define SYS_bind 49
%define SYS_listen 50
%define SYS_setsockopt 54
%define SYS_exit_group 231
%define SYS_time 201
%define SYS_prlimit64 302

extern sqlite3_prepare_v3, sqlite3_step, sqlite3_reset, sqlite3_bind_int64, sqlite3_bind_text
extern sqlite3_column_int64, sqlite3_column_text, sqlite3_column_bytes, sqlite3_changes
extern sqlite3_get_autocommit, sqlite3_errmsg
; shim.c
extern td_open, td_begin, td_end, td_first, td_next, td_seek, td_rec, td_likes_from, td_page, td_likes_setix
extern td_db, td_bt_ok, td_wal_frames, td_wal_asks, td_wal_replies, td_kick_fd, td_walkick_mb
; libc
extern malloc, realloc, free, memmove, abort, exit, time, clock_gettime, dprintf, strtod

; -------- literal/string helpers --------
%macro DEFSTR 1-*
%1: db %{2:-1}
%1 %+ _len equ $-%1
%endmacro

%macro PUTL 1
  lea rsi, [%1]
  mov edx, %1 %+ _len
  call a_emit
%endmacro

section .rodata
DEFSTR s_200, "HTTP/1.1 200 OK",13,10
DEFSTR s_201, "HTTP/1.1 201 Created",13,10
DEFSTR s_400, "HTTP/1.1 400 Bad Request",13,10
DEFSTR s_401, "HTTP/1.1 401 Unauthorized",13,10
DEFSTR s_404, "HTTP/1.1 404 Not Found",13,10
DEFSTR s_503, "HTTP/1.1 503 Service Unavailable",13,10
DEFSTR s_500, "HTTP/1.1 500 Internal Server Error",13,10
DEFSTR s_conn_close, "Connection: close",13,10
DEFSTR s_ctype, "Content-Type: application/json",13,10,"Content-Length:       ",13,10,13,10
DEFSTR s_100cont, "HTTP/1.1 100 Continue",13,10,13,10

DEFSTR e_notfound, '{"error":"not found"}'
DEFSTR e_badid, '{"error":"invalid post id"}'
DEFSTR e_nopost, '{"error":"post not found"}'
DEFSTR e_nobearer, '{"error":"missing bearer token"}'
DEFSTR e_badtoken, '{"error":"invalid or expired token"}'
DEFSTR e_payload, '{"error":"invalid token payload"}'
DEFSTR e_badjson, '{"error":"malformed JSON body"}'
DEFSTR e_bodyreq, '{"error":"body is required"}'
DEFSTR e_bodylong, '{"error":"body must be at most 500 characters"}'
DEFSTR e_500, '{"error":"internal server error"}'

DEFSTR j_postsarr, '{"posts":['
DEFSTR j_postobj, '{"post":'
DEFSTR j_id, '{"id":'
DEFSTR j_body, ',"body":"'
DEFSTR j_created, '","created_at":"'
DEFSTR j_author, '","author":"'
DEFSTR j_likec, '","like_count":'
DEFSTR j_closeobj, '}'
DEFSTR j_closearr, ']}'
DEFSTR j_liked, '{"liked":true,"already_liked":'
DEFSTR j_true, 'true'
DEFSTR j_false, 'false'
DEFSTR j_postid, ',"post_id":'
DEFSTR j_health_ok, '{"status":"ok","db":"ok","uptime_s":'
DEFSTR j_health_bad, '{"status":"degraded","db":"unreachable","error":"'
DEFSTR j_qemark, '"}'
DEFSTR j_newpost, '{"post":{"id":'
DEFSTR j_np_like0, '","like_count":0}}'

DEFSTR sql_feed, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20",0
DEFSTR sql_post, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?",0
DEFSTR sql_ins, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",0
DEFSTR sql_like, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING",0
DEFSTR sql_like_bt, "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",0
DEFSTR sql_one, "SELECT 1",0
DEFSTR sql_begin, "BEGIN",0
DEFSTR sql_commit, "COMMIT",0
DEFSTR sql_rollback, "ROLLBACK",0
DEFSTR s_nouring, "io_uring setup failed (needs Linux 6.1+ with io_uring enabled)",10
DEFSTR s_bindfail, "bind/listen failed",10
DEFSTR s_addring, "%u connections: added ring %d",10,0
DEFSTR s_enterfail, "io_uring_enter: error %d",10,0

DEFSTR env_sqlite, "SQLITE_PATH"
DEFSTR env_secret, "JWT_SECRET"
DEFSTR env_host, "HOST"
DEFSTR env_port, "PORT"
DEFSTR default_host, "0.0.0.0",0

; method/path match literals
DEFSTR m_feed, "/feed"
DEFSTR m_posts7, "/posts/"
DEFSTR m_posts, "/posts"
DEFSTR m_like, "/like"
DEFSTR m_health, "/health"
DEFSTR k_alg, "alg"
DEFSTR k_exp, "exp"
DEFSTR k_nbf, "nbf"
DEFSTR k_sub, "sub"
DEFSTR k_username, "username"
DEFSTR k_body, "body"
DEFSTR v_hs256, "HS256"
DEFSTR h_contentlength, "content-length"
DEFSTR h_authorization, "authorization"
DEFSTR h_connection, "connection"
DEFSTR h_expect, "expect"
DEFSTR h_te, "transfer-encoding"
DEFSTR v_close, "close"
DEFSTR v_keepalive, "keep-alive"
DEFSTR v_100continue, "100-continue"
DEFSTR bearer, "Bearer "
DEFSTR b64alpha, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
DEFSTR hexdigits, "0123456789abcdef"

align 16
js_lo:    times 16 db 0x1f     ; a_jstr: bytes below 0x20, '"' and '\' need escaping
js_q:     times 16 db '"'
js_bs:    times 16 db '\'
K256:
dd 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5
dd 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174
dd 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da
dd 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967
dd 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85
dd 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070
dd 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3
dd 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
H256init:
dd 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
zeros128: times 128 db 0
wait_ts:  dq 0, 2000000        ; __kernel_timespec: a busy pass waits at most 2 ms
wait_arg: dq 0, 0, wait_ts     ; io_uring_getevents_arg: sigmask, sigmask_sz + pad, ts

section .bss
alignb 16
rings:    resb MAXRINGS*128
arena:    resb BATCH_OUT
segs:     resb MAXSEG*16        ; {who q, off d, len d}
txtbuf:   resb MAXREQ           ; decoded body
unamebuf: resb 1024
numbuf:   resb 64

; HMAC key schedule: sha state (h[8]=32, n=8, b[64], bl=4) = 108 bytes, pad to 112
align 16
hm_in:    resb 112
hm_out:   resb 112
b64tab:   resb 256

alignb 8
st_feed:  resq 1
st_post:  resq 1
st_ins:   resq 1
st_like:  resq 1
st_one:   resq 1
st_begin: resq 1
st_commit: resq 1
st_rollback: resq 1
awr:      resq 1                 ; arena write cursor
cl_pos:   resq 1                 ; Content-Length patch position
body_start: resq 1
nsegs:    resd 1
ndef:     resd 1
arena_full: resd 1
in_txn:   resd 1
txn_writes: resd 1
batch_failed: resd 1
closing_flag: resd 1
g_port:   resd 1
g_host:   resq 1
envp_ptr: resq 1
started:  resq 1
now_s:    resq 1
sockaddr: resb 16
one_val:  resd 1

section .text
global main

; ---------- arena emit ----------
; a_emit: rsi=src, rdx=len. appends to arena. clobbers rax,rcx,rdi,rsi.
a_emit:
  mov rdi, [awr]
  mov rcx, rdx
  rep movsb
  mov [awr], rdi
  ret

; a_char: dil=byte
a_char:
  mov rax, [awr]
  mov [rax], dil
  inc rax
  mov [awr], rax
  ret

; a_u64: rdi=value, append decimal. clobbers rax,rcx,rdx,rsi,r8,r9
a_u64:
  mov rax, rdi
  lea r8, [numbuf+64]
  mov r9, r8
  mov rcx, 10
.lp:
  xor rdx, rdx
  div rcx
  add dl, '0'
  dec r9
  mov [r9], dl
  test rax, rax
  jnz .lp
  mov rsi, r9
  mov rdx, r8
  sub rdx, r9
  jmp a_emit

; a_jstr: rsi=ptr, rdx=len. append JSON-escaped string contents. clobbers many.
a_jstr:
  push rbx
  push r12
  push r13
  push r14
  mov r12, rsi            ; cur
  lea r13, [rsi+rdx]      ; end
  mov r14, rsi            ; run start
.loop:
  mov rax, r13
  sub rax, r12
  cmp rax, 16
  jb .byte
  movdqu xmm0, [r12]           ; 16 bytes at a time up to the next one that needs escaping
  movdqa xmm1, xmm0
  pminub xmm1, [js_lo]
  pcmpeqb xmm1, xmm0           ; byte <= 0x1f
  movdqa xmm2, xmm0
  pcmpeqb xmm2, [js_q]
  pcmpeqb xmm0, [js_bs]
  por xmm1, xmm2
  por xmm1, xmm0
  pmovmskb eax, xmm1
  test eax, eax
  jnz .found
  add r12, 16
  jmp .loop
.found:
  bsf eax, eax
  add r12, rax
  jmp .esc
.byte:
  cmp r12, r13
  jae .flushrun
  movzx eax, byte [r12]
  cmp al, 0x20
  jb .esc
  cmp al, '"'
  je .esc
  cmp al, '\'
  je .esc
  inc r12
  jmp .loop
.esc:
  ; flush [r14, r12)
  mov rsi, r14
  mov rdx, r12
  sub rdx, r14
  test rdx, rdx
  jz .noflush
  call a_emit
.noflush:
  lea r14, [r12+1]
  movzx ebx, byte [r12]
  ; backslash + something
  mov dil, '\'
  call a_char
  cmp bl, '"'
  je .q
  cmp bl, '\'
  je .bs
  cmp bl, 10
  je .n
  cmp bl, 13
  je .r
  cmp bl, 9
  je .t
  ; \u00XX
  mov dil, 'u'
  call a_char
  mov dil, '0'
  call a_char
  mov dil, '0'
  call a_char
  mov eax, ebx
  shr eax, 4
  lea rcx, [hexdigits]
  movzx edi, byte [rcx+rax]
  call a_char
  mov eax, ebx
  and eax, 15
  lea rcx, [hexdigits]
  movzx edi, byte [rcx+rax]
  call a_char
  jmp .adv
.q:
  mov dil, '"'
  call a_char
  jmp .adv
.bs:
  mov dil, '\'
  call a_char
  jmp .adv
.n:
  mov dil, 'n'
  call a_char
  jmp .adv
.r:
  mov dil, 'r'
  call a_char
  jmp .adv
.t:
  mov dil, 't'
  call a_char
.adv:
  inc r12
  jmp .loop
.flushrun:
  mov rsi, r14
  mov rdx, r13
  sub rdx, r14
  test rdx, rdx
  jz .done
  call a_emit
.done:
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ---------- response helpers ----------
; begin_resp: edi=status, esi=close(0/1). writes status line + headers, sets cl_pos/body_start
begin_resp:
  push rbx
  push r12
  mov r12d, esi          ; close
  ; status line
  cmp edi, 200
  jne .n201
  PUTL s_200
  jmp .hdr
.n201:
  cmp edi, 201
  jne .n400
  PUTL s_201
  jmp .hdr
.n400:
  cmp edi, 400
  jne .n401
  PUTL s_400
  jmp .hdr
.n401:
  cmp edi, 401
  jne .n404
  PUTL s_401
  jmp .hdr
.n404:
  cmp edi, 404
  jne .n503
  PUTL s_404
  jmp .hdr
.n503:
  cmp edi, 503
  jne .n500
  PUTL s_503
  jmp .hdr
.n500:
  PUTL s_500
.hdr:
  test r12d, r12d
  jz .noclose
  PUTL s_conn_close
.noclose:
  PUTL s_ctype
  mov rax, [awr]
  sub rax, 10
  mov [cl_pos], rax
  mov rax, [awr]
  mov [body_start], rax
  pop r12
  pop rbx
  ret

; finish: patch content-length
finish:
  mov rax, [awr]
  sub rax, [body_start]      ; n
  mov rcx, [cl_pos]
  add rcx, 6                 ; p
  mov r8, 10
.lp:
  xor rdx, rdx
  div r8
  add dl, '0'
  dec rcx
  mov [rcx], dl
  test rax, rax
  jnz .lp
  ret

; send_err: edi=status, rsi=errlit ptr, edx=errlit len, ecx=close
send_err:
  push rbx
  push r12
  push r13
  mov r12, rsi
  mov r13, rdx
  mov ebx, ecx
  ; begin_resp(status, close)
  mov esi, ebx
  call begin_resp
  mov rsi, r12
  mov rdx, r13
  call a_emit
  call finish
  pop r13
  pop r12
  pop rbx
  ret

; macro: emit error and return from handle. %1=status %2=label %3 uses close in r15b
%macro ERR 2
  mov edi, %1
  lea rsi, [%2]
  mov edx, %2 %+ _len
  movzx ecx, r15b
  call send_err
  jmp .ret
%endmacro

; ---------- SHA-256 ----------
; sha_block: rdi=state(h[8]), rsi=ptr(64 bytes). clobbers volatile regs + uses stack for w[64]
sha_block:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 256+16           ; w[64]
  mov r14, rdi              ; state
  mov r15, rsi              ; block
  ; load w[0..15] big-endian
  xor rcx, rcx
.load:
  mov eax, [r15 + rcx*4]
  bswap eax
  mov [rsp + rcx*4], eax
  inc rcx
  cmp rcx, 16
  jb .load
  ; extend
  mov rcx, 16
.ext:
  mov eax, [rsp + (rcx-15)*4]      ; w[i-15]
  mov ebx, eax
  mov edx, eax
  ror eax, 7
  ror ebx, 18
  shr edx, 3
  xor eax, ebx
  xor eax, edx                     ; s0
  mov r8d, [rsp + (rcx-2)*4]
  mov r9d, r8d
  mov r10d, r8d
  ror r8d, 17
  ror r9d, 19
  shr r10d, 10
  xor r8d, r9d
  xor r8d, r10d                    ; s1
  add eax, r8d
  add eax, [rsp + (rcx-16)*4]
  add eax, [rsp + (rcx-7)*4]
  mov [rsp + rcx*4], eax
  inc rcx
  cmp rcx, 64
  jb .ext
  ; working vars a..h in regs: eax=a ebx=b ecx=c edx=d r8=e r9=f r10=g r11=h
  mov eax, [r14+0]
  mov ebx, [r14+4]
  mov ecx, [r14+8]
  mov edx, [r14+12]
  mov r8d, [r14+16]
  mov r9d, [r14+20]
  mov r10d, [r14+24]
  mov r11d, [r14+28]
  xor rbp, rbp                     ; i
.round:
  ; S1 = ror(e,6)^ror(e,11)^ror(e,25)
  mov r12d, r8d
  mov r13d, r8d
  mov r15d, r8d
  ror r12d, 6
  ror r13d, 11
  ror r15d, 25
  xor r12d, r13d
  xor r12d, r15d                   ; S1
  ; ch = (e&f)^(~e&g)
  mov r13d, r8d
  and r13d, r9d
  mov r15d, r8d
  not r15d
  and r15d, r10d
  xor r13d, r15d                   ; ch
  ; t1 = h + S1 + ch + K[i] + w[i]
  add r11d, r12d
  add r11d, r13d
  lea r15, [K256]
  mov r12d, [r15 + rbp*4]
  add r11d, r12d
  mov r12d, [rsp + rbp*4]
  add r11d, r12d                   ; t1 in r11d
  ; S0 = ror(a,2)^ror(a,13)^ror(a,22)
  mov r12d, eax
  mov r13d, eax
  mov r15d, eax
  ror r12d, 2
  ror r13d, 13
  ror r15d, 22
  xor r12d, r13d
  xor r12d, r15d                   ; S0
  ; maj = (a&b)^(a&c)^(b&c)
  mov r13d, eax
  and r13d, ebx
  mov r15d, eax
  and r15d, ecx
  xor r13d, r15d
  mov r15d, ebx
  and r15d, ecx
  xor r13d, r15d                   ; maj
  add r12d, r13d                   ; t2 = S0+maj
  ; rotate: h=g g=f f=e e=d+t1 d=c c=b b=a a=t1+t2
  mov r13d, r11d                   ; save t1
  mov r11d, r10d                   ; h=g
  mov r10d, r9d                    ; g=f
  mov r9d, r8d                     ; f=e
  mov r8d, edx
  add r8d, r13d                    ; e=d+t1
  mov edx, ecx                     ; d=c
  mov ecx, ebx                     ; c=b
  mov ebx, eax                     ; b=a
  mov eax, r13d
  add eax, r12d                    ; a=t1+t2
  inc rbp
  cmp rbp, 64
  jb .round
  add [r14+0], eax
  add [r14+4], ebx
  add [r14+8], ecx
  add [r14+12], edx
  add [r14+16], r8d
  add [r14+20], r9d
  add [r14+24], r10d
  add [r14+28], r11d
  add rsp, 256+16
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; sha_init: rdi=ctx (h[8],n q,b[64],bl d)
sha_init:
  lea rsi, [H256init]
  mov ecx, 8
  rep movsd
  mov qword [rdi], 0          ; n
  mov dword [rdi+72], 0       ; bl (after b[64])
  ret

; ctx layout: +0 h[8] (32), +32 n (8), +40 b[64], +104 bl (4)
%define SHA_N 32
%define SHA_B 40
%define SHA_BL 104

; sha_upd: rdi=ctx, rsi=ptr, rdx=len
sha_upd:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, rdi
  mov r13, rsi
  mov r14, rdx
  add [r12+SHA_N], r14
  mov r15d, [r12+SHA_BL]
  test r15d, r15d
  jz .bulk
  ; fill partial block
  mov eax, 64
  sub eax, r15d              ; t = 64-bl
  cmp rax, r14
  jbe .t_ok
  mov eax, r14d
.t_ok:
  ; copy rax bytes from r13 to b+bl
  lea rdi, [r12+SHA_B]
  add rdi, r15
  mov rsi, r13
  mov rcx, rax
  push rax
  rep movsb
  pop rax
  add r13, rax
  sub r14, rax
  add r15d, eax
  mov [r12+SHA_BL], r15d
  cmp r15d, 64
  jne .bulk
  lea rdi, [r12]
  lea rsi, [r12+SHA_B]
  call sha_block
  mov dword [r12+SHA_BL], 0
.bulk:
  ; while n>=64
.bl_loop:
  cmp r14, 64
  jb .tail
  lea rdi, [r12]
  mov rsi, r13
  call sha_block
  add r13, 64
  sub r14, 64
  jmp .bl_loop
.tail:
  test r14, r14
  jz .done
  lea rdi, [r12+SHA_B]
  mov rsi, r13
  mov rcx, r14
  rep movsb
  mov [r12+SHA_BL], r14d
.done:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; sha_fin: rdi=ctx, rsi=out(32)
sha_fin:
  push rbx
  push r12
  push r13
  sub rsp, 32
  mov r12, rdi
  mov r13, rsi
  mov rax, [r12+SHA_N]
  lea rax, [rax*8]           ; bits
  mov [rsp], rax             ; save bits
  ; pad 0x80
  mov byte [rsp+16], 0x80
  mov rdi, r12
  lea rsi, [rsp+16]
  mov rdx, 1
  call sha_upd
  ; zeros until bl==56 (mod) : compute count = (bl<=56?56:120)-bl
  mov eax, [r12+SHA_BL]
  cmp eax, 56
  jbe .le
  mov edx, 120
  jmp .sub
.le:
  mov edx, 56
.sub:
  sub edx, eax               ; count
  test edx, edx
  jz .lenpad
  mov rdi, r12
  lea rsi, [zeros128]
  movsxd rdx, edx
  call sha_upd
.lenpad:
  ; length big-endian 8 bytes
  mov rax, [rsp]             ; bits
  bswap rax
  mov [rsp+16], rax
  mov rdi, r12
  lea rsi, [rsp+16]
  mov rdx, 8
  call sha_upd
  ; output h[] big-endian
  xor rcx, rcx
.out:
  mov eax, [r12 + rcx*4]
  bswap eax
  mov [r13 + rcx*4], eax
  inc rcx
  cmp rcx, 8
  jb .out
  add rsp, 32
  pop r13
  pop r12
  pop rbx
  ret

; hmac_key: rdi=key ptr (NUL-terminated)
hmac_key:
  push rbx
  push r12
  push r13
  sub rsp, 160               ; k[64] at rsp, pad[64] at rsp+64
  mov r12, rdi
  ; strlen
  xor rcx, rcx
.sl:
  cmp byte [r12+rcx], 0
  je .sld
  inc rcx
  jmp .sl
.sld:
  mov r13, rcx               ; kl
  ; zero k[64]
  lea rdi, [rsp]
  mov rcx, 64
  xor al, al
  rep stosb
  cmp r13, 64
  jbe .copyk
  ; k = sha(key)
  lea rdi, [hm_in]           ; reuse hm_in as scratch ctx temporarily
  call sha_init
  mov rdi, hm_in
  mov rsi, r12
  mov rdx, r13
  call sha_upd
  mov rdi, hm_in
  lea rsi, [rsp]
  call sha_fin
  jmp .pads
.copyk:
  mov rsi, r12
  lea rdi, [rsp]
  mov rcx, r13
  rep movsb
.pads:
  ; pad = k ^ 0x36 ; init hm_in; upd pad
  xor rcx, rcx
.p1:
  movzx eax, byte [rsp+rcx]
  xor al, 0x36
  mov [rsp+64+rcx], al
  inc rcx
  cmp rcx, 64
  jb .p1
  mov rdi, hm_in
  call sha_init
  mov rdi, hm_in
  lea rsi, [rsp+64]
  mov rdx, 64
  call sha_upd
  ; pad = k ^ 0x5c ; init hm_out; upd
  xor rcx, rcx
.p2:
  movzx eax, byte [rsp+rcx]
  xor al, 0x5c
  mov [rsp+64+rcx], al
  inc rcx
  cmp rcx, 64
  jb .p2
  mov rdi, hm_out
  call sha_init
  mov rdi, hm_out
  lea rsi, [rsp+64]
  mov rdx, 64
  call sha_upd
  add rsp, 160
  pop r13
  pop r12
  pop rbx
  ret

; hmac: rsi=msg, rdx=len, rdi=out(32). clobbers volatile. uses stack ctx
hmac:
  push rbx
  push r12
  sub rsp, 160               ; ctx(112) + ih(32)
  mov rbx, rdi               ; out
  mov r12, rsi
  ; ctx = hm_in
  lea rdi, [rsp]
  lea rsi, [hm_in]
  mov rcx, 112
  rep movsb
  ; upd(ctx, msg, len)
  lea rdi, [rsp]
  mov rsi, r12
  ; len still in rdx
  call sha_upd
  ; fin -> ih at rsp+112
  lea rdi, [rsp]
  lea rsi, [rsp+112]
  call sha_fin
  ; ctx = hm_out
  lea rdi, [rsp]
  lea rsi, [hm_out]
  mov rcx, 112
  rep movsb
  lea rdi, [rsp]
  lea rsi, [rsp+112]
  mov rdx, 32
  call sha_upd
  lea rdi, [rsp]
  mov rsi, rbx
  call sha_fin
  add rsp, 160
  pop r12
  pop rbx
  ret

; ---------- base64url decode ----------
; b64dec: rdi=src, rsi=len, rdx=out -> rax=outlen or -1
; registers: r12=src r13=len r14=out  r8=acc r9d=bits r10=o r11=i
b64dec:
  push rbx
  push r12
  push r13
  push r14
  mov r12, rdi
  mov r13, rsi
  mov r14, rdx
  mov rax, rsi
  and rax, 3
  cmp rax, 1
  je .bad
  xor r8, r8                 ; acc
  xor r9d, r9d               ; bits
  xor r10, r10               ; o
  xor r11, r11               ; i
.lp:
  cmp r11, r13
  jae .done
  movzx eax, byte [r12+r11]
  lea rbx, [b64tab]
  movsx eax, byte [rbx+rax]
  test eax, eax
  js .bad
  shl r8, 6
  or r8, rax
  add r9d, 6
  cmp r9d, 8
  jb .next
  sub r9d, 8
  mov rax, r8
  mov ecx, r9d
  shr rax, cl                ; acc >> bits
  mov [r14+r10], al
  inc r10
.next:
  inc r11
  jmp .lp
.done:
  mov rax, r10
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.bad:
  mov rax, -1
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ---------- JSON ----------
; js struct: [0]=p (cur), [8]=e (end)
; ws: rdi=js*
jws:
  mov rax, [rdi]
  mov rdx, [rdi+8]
.l:
  cmp rax, rdx
  jae .d
  movzx ecx, byte [rax]
  cmp cl, ' '
  je .adv
  cmp cl, 9
  je .adv
  cmp cl, 10
  je .adv
  cmp cl, 13
  jne .d
.adv:
  inc rax
  jmp .l
.d:
  mov [rdi], rax
  ret

; hexv: dil=char -> eax value or -1
hexv:
  movzx eax, dil
  cmp al, '0'
  jb .bad
  cmp al, '9'
  ja .af
  sub eax, '0'
  ret
.af:
  cmp al, 'a'
  jb .AF
  cmp al, 'f'
  ja .bad
  sub eax, 'a'-10
  ret
.AF:
  cmp al, 'A'
  jb .bad
  cmp al, 'F'
  ja .bad
  sub eax, 'A'-10
  ret
.bad:
  mov eax, -1
  ret

; utf8_len: rdi=p, rsi=e -> rax=len(1..4) or 0. clobbers only rax,rcx,rdx.
utf8_len:
  movzx eax, byte [rdi]
  cmp al, 0x80
  jb .one
  cmp al, 0xc2
  jb .zero
  cmp al, 0xdf
  ja .three
  ; 2-byte
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 2
  jb .zero
  movzx edx, byte [rdi+1]
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  mov eax, 2
  ret
.three:
  cmp al, 0xef
  ja .four
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 3
  jb .zero
  movzx edx, byte [rdi+1]
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  movzx edx, byte [rdi+2]
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  movzx edx, byte [rdi+1]
  cmp al, 0xe0
  jne .n_ed
  cmp dl, 0xa0
  jb .zero
.n_ed:
  cmp al, 0xed
  jne .ok3
  cmp dl, 0x9f
  ja .zero
.ok3:
  mov eax, 3
  ret
.four:
  cmp al, 0xf4
  ja .zero
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 4
  jb .zero
  movzx edx, byte [rdi+2]
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  movzx edx, byte [rdi+3]
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  movzx edx, byte [rdi+1]
  mov cl, dl
  and dl, 0xc0
  cmp dl, 0x80
  jne .zero
  cmp al, 0xf0
  jne .n_f0
  cmp cl, 0x90
  jb .zero
.n_f0:
  cmp al, 0xf4
  jne .ok4
  cmp cl, 0x8f
  ja .zero
.ok4:
  mov eax, 4
  ret
.one:
  mov eax, 1
  ret
.zero:
  xor eax, eax
  ret

; emit a decoded codepoint (eax=cp) to out buffer at [r14+r13], bump r13, cap check against r15.
; returns CF set on overflow. used only inside jstr. clobbers rcx,rdx,r8
%macro CP_EMIT 0
  ; eax=cp
  cmp eax, 0x80
  jae %%m2
  ; 1 byte
  lea rcx, [r13+1]
  cmp rcx, r15
  ja %%ovf
  mov [r14+r13], al
  mov r13, rcx
  jmp %%done
%%m2:
  cmp eax, 0x800
  jae %%m3
  lea rcx, [r13+2]
  cmp rcx, r15
  ja %%ovf
  mov edx, eax
  shr edx, 6
  or dl, 0xc0
  mov [r14+r13], dl
  mov edx, eax
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+1], dl
  mov r13, rcx
  jmp %%done
%%m3:
  cmp eax, 0x10000
  jae %%m4
  lea rcx, [r13+3]
  cmp rcx, r15
  ja %%ovf
  mov edx, eax
  shr edx, 12
  or dl, 0xe0
  mov [r14+r13], dl
  mov edx, eax
  shr edx, 6
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+1], dl
  mov edx, eax
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+2], dl
  mov r13, rcx
  jmp %%done
%%m4:
  lea rcx, [r13+4]
  cmp rcx, r15
  ja %%ovf
  mov edx, eax
  shr edx, 18
  or dl, 0xf0
  mov [r14+r13], dl
  mov edx, eax
  shr edx, 12
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+1], dl
  mov edx, eax
  shr edx, 6
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+2], dl
  mov edx, eax
  and edx, 0x3f
  or dl, 0x80
  mov [r14+r13+3], dl
  mov r13, rcx
  jmp %%done
%%ovf:
  stc
  jmp %%out
%%done:
  clc
%%out:
%endmacro

; jstr: rdi=js*, rsi=out(0=validate), rdx=cap -> rax=len/-1/-2. updates [rdi] past closing quote.
; regs: rbx=js* r12=p r14=out r15=cap r13=o r8=e
jstr:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi
  mov r14, rsi
  mov r15, rdx
  mov r12, [rbx]
  inc r12                   ; skip opening quote
  mov r8, [rbx+8]           ; e
  xor r13, r13              ; o
  xor r11d, r11d            ; over flag: out buffer too small, keep validating
.loop:
  cmp r12, r8
  jae .synbad
  movzx eax, byte [r12]
  cmp al, '"'
  je .close
  cmp al, 0x20
  jb .synbad
  cmp al, '\'
  je .esc
  ; normal UTF-8 run
  mov rdi, r12
  mov rsi, r8
  call utf8_len
  test eax, eax
  jz .synbad
  movsxd r9, eax            ; l
  test r14, r14
  jz .skip_copy
  lea rcx, [r13+r9]
  cmp rcx, r15
  ja .ov_utf8
  ; copy r9 bytes
  mov rdi, r14
  add rdi, r13
  mov rsi, r12
  mov rcx, r9
  rep movsb
.skip_copy:
  add r13, r9
  add r12, r9
  jmp .loop
.esc:
  inc r12
  cmp r12, r8
  jae .synbad
  movzx eax, byte [r12]
  cmp al, '"'
  je .e_dq
  cmp al, '\'
  je .e_bs
  cmp al, '/'
  je .e_sl
  cmp al, 'b'
  je .e_b
  cmp al, 'f'
  je .e_f
  cmp al, 'n'
  je .e_n
  cmp al, 'r'
  je .e_r
  cmp al, 't'
  je .e_t
  cmp al, 'u'
  je .e_u
  jmp .synbad
.e_dq:
  mov al, '"'
  jmp .e_put
.e_bs:
  mov al, '\'
  jmp .e_put
.e_sl:
  mov al, '/'
  jmp .e_put
.e_b:
  mov al, 8
  jmp .e_put
.e_f:
  mov al, 12
  jmp .e_put
.e_n:
  mov al, 10
  jmp .e_put
.e_r:
  mov al, 13
  jmp .e_put
.e_t:
  mov al, 9
.e_put:
  test r14, r14
  jz .e_put_skip
  cmp r13, r15
  jae .ov_esc
  mov [r14+r13], al
.e_put_skip:
  inc r13
  inc r12
  jmp .loop
.e_u:
  ; need 4 hex after 'u'
  mov rcx, r8
  sub rcx, r12
  cmp rcx, 5
  jb .synbad
  ; decode 4 hex into eax (cp)
  movzx edi, byte [r12+1]
  call hexv
  test eax, eax
  js .synbad
  mov r9d, eax
  shl r9d, 12
  movzx edi, byte [r12+2]
  call hexv
  test eax, eax
  js .synbad
  shl eax, 8
  or r9d, eax
  movzx edi, byte [r12+3]
  call hexv
  test eax, eax
  js .synbad
  shl eax, 4
  or r9d, eax
  movzx edi, byte [r12+4]
  call hexv
  test eax, eax
  js .synbad
  or r9d, eax               ; cp in r9d
  add r12, 4                ; now at the last hex digit
  ; surrogate pair?
  cmp r9d, 0xd800
  jb .u_done
  cmp r9d, 0xdbff
  ja .u_done
  ; need "\uXXXX" after: e-p>=7 (p currently at last hex of first escape)
  mov rcx, r8
  sub rcx, r12
  cmp rcx, 7
  jb .u_lone
  cmp byte [r12+1], '\'
  jne .u_lone
  cmp byte [r12+2], 'u'
  jne .u_lone
  movzx edi, byte [r12+3]
  call hexv
  test eax, eax
  js .u_lone
  mov r10d, eax
  shl r10d, 12
  movzx edi, byte [r12+4]
  call hexv
  test eax, eax
  js .u_lone
  shl eax, 8
  or r10d, eax
  movzx edi, byte [r12+5]
  call hexv
  test eax, eax
  js .u_lone
  shl eax, 4
  or r10d, eax
  movzx edi, byte [r12+6]
  call hexv
  test eax, eax
  js .u_lone
  or r10d, eax              ; lo
  cmp r10d, 0xdc00
  jb .u_lone
  cmp r10d, 0xdfff
  ja .u_lone
  ; combine
  mov eax, r9d
  sub eax, 0xd800
  shl eax, 10
  mov edx, r10d
  sub edx, 0xdc00
  add eax, edx
  add eax, 0x10000
  mov r9d, eax
  add r12, 6
  jmp .u_emit
.u_lone:
  mov r9d, 0xfffd           ; lone surrogate -> U+FFFD
  jmp .u_emit
.u_done:
.u_emit:
  mov eax, r9d
  test r14, r14
  jnz .u_write
  ; validate-only: count the code point's UTF-8 length
  cmp eax, 0x80
  jb .cl1
  cmp eax, 0x800
  jb .cl2
  cmp eax, 0x10000
  jb .cl3
  add r13, 4
  jmp .u_after
.cl1:
  inc r13
  jmp .u_after
.cl2:
  add r13, 2
  jmp .u_after
.cl3:
  add r13, 3
  jmp .u_after
.u_write:
  CP_EMIT
  jc .ov_u
.u_after:
  inc r12                  ; move past last hex char
  jmp .loop
.close:
  inc r12
  mov [rbx], r12
  mov rax, r13
  test r11d, r11d
  jz .ret
  mov rax, -2
  jmp .ret
.ov_utf8:
  xor r14d, r14d
  mov r11d, 1
  jmp .skip_copy
.ov_esc:
  xor r14d, r14d
  mov r11d, 1
  jmp .e_put_skip
.ov_u:
  xor r14d, r14d
  mov r11d, 1
  jmp .u_emit
.synbad:
  mov rax, -1
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; jnumber: rdi=js* -> rax=1/0, updates p
jnumber:
  mov rax, [rdi]
  mov r8, [rdi+8]
  cmp rax, r8
  jae .bad
  cmp byte [rax], '-'
  jne .int
  inc rax
  cmp rax, r8
  jae .bad
.int:
  movzx ecx, byte [rax]
  cmp cl, '0'
  jne .nonzero
  inc rax
  jmp .frac
.nonzero:
  cmp cl, '1'
  jb .bad
  cmp cl, '9'
  ja .bad
.digs:
  inc rax
  cmp rax, r8
  jae .frac
  movzx ecx, byte [rax]
  cmp cl, '0'
  jb .frac
  cmp cl, '9'
  ja .frac
  jmp .digs
.frac:
  cmp rax, r8
  jae .ok
  cmp byte [rax], '.'
  jne .exp
  inc rax
  cmp rax, r8
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, '0'
  jb .bad
  cmp cl, '9'
  ja .bad
.fdigs:
  inc rax
  cmp rax, r8
  jae .ok
  movzx ecx, byte [rax]
  cmp cl, '0'
  jb .exp
  cmp cl, '9'
  ja .exp
  jmp .fdigs
.exp:
  cmp rax, r8
  jae .ok
  movzx ecx, byte [rax]
  cmp cl, 'e'
  je .edo
  cmp cl, 'E'
  jne .ok
.edo:
  inc rax
  cmp rax, r8
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, '+'
  je .esign
  cmp cl, '-'
  jne .edig1
.esign:
  inc rax
  cmp rax, r8
  jae .bad
  movzx ecx, byte [rax]
.edig1:
  cmp cl, '0'
  jb .bad
  cmp cl, '9'
  ja .bad
.edigs:
  inc rax
  cmp rax, r8
  jae .ok
  movzx ecx, byte [rax]
  cmp cl, '0'
  jb .ok
  cmp cl, '9'
  ja .ok
  jmp .edigs
.ok:
  mov [rdi], rax
  mov eax, 1
  ret
.bad:
  xor eax, eax
  ret

; jvalue: rdi=js*, esi=depth -> rax=1/0. recursive.
jvalue:
  push rbx
  push r12
  mov rbx, rdi
  cmp esi, 64
  jg .bad
  mov r12d, esi
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, '"'
  je .str
  cmp cl, '{'
  je .obj
  cmp cl, '['
  je .arr
  cmp cl, 't'
  je .t
  cmp cl, 'f'
  je .f
  cmp cl, 'n'
  je .n
  mov rdi, rbx
  call jnumber
  jmp .ret
.str:
  mov rdi, rbx
  xor rsi, rsi
  xor rdx, rdx
  call jstr
  test rax, rax
  jns .ok
  jmp .bad
.obj:
  mov rax, [rbx]
  inc rax
  mov [rbx], rax
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .objloop
  cmp byte [rax], '}'
  jne .objloop
  inc rax
  mov [rbx], rax
  jmp .ok
.objloop:
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .bad
  cmp byte [rax], '"'
  jne .bad
  mov rdi, rbx
  xor rsi, rsi
  xor rdx, rdx
  call jstr
  cmp rax, 0
  jl .bad
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .bad
  cmp byte [rax], ':'
  jne .bad
  inc rax
  mov [rbx], rax
  lea esi, [r12+1]
  mov rdi, rbx
  call jvalue
  test rax, rax
  jz .bad
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, ','
  jne .obj_rb
  inc rax
  mov [rbx], rax
  jmp .objloop
.obj_rb:
  cmp cl, '}'
  jne .bad
  inc rax
  mov [rbx], rax
  jmp .ok
.arr:
  mov rax, [rbx]
  inc rax
  mov [rbx], rax
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .arrloop
  cmp byte [rax], ']'
  jne .arrloop
  inc rax
  mov [rbx], rax
  jmp .ok
.arrloop:
  lea esi, [r12+1]
  mov rdi, rbx
  call jvalue
  test rax, rax
  jz .bad
  mov rdi, rbx
  call jws
  mov rax, [rbx]
  cmp rax, [rbx+8]
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, ','
  jne .arr_rb
  inc rax
  mov [rbx], rax
  jmp .arrloop
.arr_rb:
  cmp cl, ']'
  jne .bad
  inc rax
  mov [rbx], rax
  jmp .ok
.t:
  lea rsi, [.lit_true]
  mov rdx, 4
  jmp .matchlit
.f:
  lea rsi, [.lit_false]
  mov rdx, 5
  jmp .matchlit
.n:
  lea rsi, [.lit_null]
  mov rdx, 4
.matchlit:
  mov rax, [rbx]
  mov rcx, [rbx+8]
  sub rcx, rax
  cmp rcx, rdx
  jb .bad
  mov rcx, rdx
  mov rdi, rax
.cmp:
  mov r8b, [rdi]
  cmp r8b, [rsi]
  jne .bad
  inc rdi
  inc rsi
  dec rcx
  jnz .cmp
  mov [rbx], rdi
.ok:
  mov eax, 1
  jmp .ret
.bad:
  xor eax, eax
.ret:
  pop r12
  pop rbx
  ret
.lit_true: db "true"
.lit_false: db "false"
.lit_null: db "null"

; jdoc: rdi=s, rsi=len, rdx=wants, ecx=nw -> rax=1/0. want entry 32B: keyptr,keylen,v,ve
; regs: rbx=wants r12=nw  uses local js on stack
jdoc:
  push rbx
  push r12
  push r13
  push r14
  push r15
  push rbp
  sub rsp, 48               ; js {p,e} at [rsp], key buffer kb[32] at [rsp+16]
  mov rbx, rdx
  mov r12d, ecx
  ; clear v for all wants
  xor rax, rax
.clr:
  cmp rax, r12
  jae .clrd
  mov r10, rax
  shl r10, 5
  mov qword [rbx + r10 + 16], 0
  inc rax
  jmp .clr
.clrd:
  mov [rsp], rdi            ; js.p = s
  lea rax, [rdi+rsi]
  mov [rsp+8], rax          ; js.e
  lea rbp, [rsp]            ; js*
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jae .topval
  cmp byte [rax], '{'
  jne .topval
  inc rax
  mov [rbp], rax
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jae .members
  cmp byte [rax], '}'
  jne .members
  inc rax
  mov [rbp], rax
  jmp .tailws
.members:
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jae .bad
  cmp byte [rax], '"'
  jne .bad
  mov rdi, rbp
  lea rsi, [rsp+16]
  mov rdx, 32
  call jstr
  cmp rax, -1
  je .bad
  mov r14, rax              ; keylen, or -2: too long for kb, so it matches no want
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jae .bad
  cmp byte [rax], ':'
  jne .bad
  inc rax
  mov [rbp], rax
  mov rdi, rbp
  call jws
  mov r15, [rbp]           ; value start (vs)
  mov rdi, rbp
  mov esi, 1
  call jvalue
  test rax, rax
  jz .bad
  mov r13, [rbp]           ; value end (ve)
  ; if keylen>=0 match against wants
  cmp r14, 0
  jl .after_match
  xor rax, rax
.mw:
  cmp rax, r12
  jae .after_match
  mov r10, rax
  shl r10, 5
  mov rcx, [rbx + r10 + 8]      ; want keylen
  cmp rcx, r14
  jne .mw_next
  mov rsi, [rbx + r10]          ; keyptr
  lea rdi, [rsp+16]
  push rax
  call memeq_n
  pop rax
  jne .mw_next
  mov [rbx + r10 + 16], r15  ; v
  mov [rbx + r10 + 24], r13  ; ve
.mw_next:
  inc rax
  jmp .mw
.after_match:
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jae .bad
  movzx ecx, byte [rax]
  cmp cl, ','
  jne .m_rb
  inc rax
  mov [rbp], rax
  jmp .members
.m_rb:
  cmp cl, '}'
  jne .bad
  inc rax
  mov [rbp], rax
  jmp .tailws
.topval:
  mov rdi, rbp
  xor esi, esi
  call jvalue
  test rax, rax
  jz .bad
.tailws:
  mov rdi, rbp
  call jws
  mov rax, [rbp]
  cmp rax, [rbp+8]
  jne .bad
  mov eax, 1
  jmp .ret
.bad:
  xor eax, eax
.ret:
  add rsp, 48
  pop rbp
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

section .bss
alignb 8
auth_uid: resq 1
auth_unl: resq 1
w_alg:    resb 32
w_pl:     resb 128
w_body:   resb 32
g_m:      resq 1
g_ml:     resq 1
g_path:   resq 1
g_pl:     resq 1
g_authh:  resq 1
g_al:     resq 1
g_body:   resq 1
g_bl:     resq 1
g_close:  resq 1
secret_ptr: resq 1

section .text

; init_wants: fill want key ptr/len
init_wants:
  lea rax, [k_alg]
  mov [w_alg], rax
  mov qword [w_alg+8], k_alg_len
  ; payload: exp,nbf,sub,username
  lea rax, [k_exp]
  mov [w_pl], rax
  mov qword [w_pl+8], k_exp_len
  lea rax, [k_nbf]
  mov [w_pl+32], rax
  mov qword [w_pl+40], k_nbf_len
  lea rax, [k_sub]
  mov [w_pl+64], rax
  mov qword [w_pl+72], k_sub_len
  lea rax, [k_username]
  mov [w_pl+96], rax
  mov qword [w_pl+104], k_username_len
  lea rax, [k_body]
  mov [w_body], rax
  mov qword [w_body+8], k_body_len
  ret

; memeq_ci: rdi=a, rsi=b, rcx=len -> ZF set if equal (case-insensitive ASCII)
memeq_ci:
  test rcx, rcx
  jz .eq
.l:
  movzx eax, byte [rdi]
  movzx edx, byte [rsi]
  or al, 0x20
  or dl, 0x20
  cmp al, dl
  jne .ne
  inc rdi
  inc rsi
  dec rcx
  jnz .l
.eq:
  xor eax, eax             ; ZF=1
  ret
.ne:
  or eax, 1                ; ZF=0
  ret

; parse_id: rdi=ptr, rsi=len -> rax=id or -1
parse_id:
  test rsi, rsi
  jz .bad
  xor rax, rax
  xor rcx, rcx
.l:
  movzx edx, byte [rdi+rcx]
  cmp dl, '0'
  jb .bad
  cmp dl, '9'
  ja .bad
  mov r9, 100000000000000000
  cmp rax, r9
  jae .cap
  imul rax, rax, 10
  sub dl, '0'
  movzx edx, dl
  add rax, rdx
  jmp .cont
.cap:
  mov rax, 0x7fffffffffffffff
.cont:
  inc rcx
  cmp rcx, rsi
  jb .l
  test rax, rax
  jle .bad
  ret
.bad:
  mov rax, -1
  ret

; jsws_at: rdi=p, rsi=e -> rax = byte len (1/2/3) or 0
jsws_at:
  movzx eax, byte [rdi]
  cmp al, ' '
  je .one
  cmp al, 9
  jb .chk2
  cmp al, 13
  jbe .one
.chk2:
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 2
  jb .zero
  cmp byte [rdi], 0xc2
  jne .chk3
  cmp byte [rdi+1], 0xa0
  jne .chk3
  mov eax, 2
  ret
.chk3:
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 3
  jb .zero
  movzx eax, byte [rdi]
  shl eax, 8
  mov al, [rdi+1]
  shl eax, 8
  mov al, [rdi+2]           ; eax = 3 bytes big-endian
  cmp eax, 0xe19a80
  je .three
  cmp eax, 0xe28080
  jb .not3
  cmp eax, 0xe2808a
  jbe .three
.not3:
  cmp eax, 0xe280a8
  je .three
  cmp eax, 0xe280a9
  je .three
  cmp eax, 0xe280af
  je .three
  cmp eax, 0xe2819f
  je .three
  cmp eax, 0xe38080
  je .three
  cmp eax, 0xefbbbf
  je .three
.zero:
  xor eax, eax
  ret
.one:
  mov eax, 1
  ret
.three:
  mov eax, 3
  ret

; jsws_before: rdi=s, rsi=e -> rax=1/2/3 or 0
jsws_before:
  movzx eax, byte [rsi-1]
  cmp al, ' '
  je .one
  cmp al, 9
  jb .c2
  cmp al, 13
  jbe .one
.c2:
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 2
  jb .c3
  cmp byte [rsi-2], 0xc2
  jne .c3
  cmp byte [rsi-1], 0xa0
  jne .c3
  mov eax, 2
  ret
.c3:
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 3
  jb .zero
  lea rdi, [rsi-3]
  ; rsi unchanged = e
  call jsws_at
  cmp rax, 3
  je .three
.zero:
  xor eax, eax
  ret
.one:
  mov eax, 1
  ret
.three:
  mov eax, 3
  ret

; auth: rdi=h, rsi=hl -> eax: 0 ok, 1 nobearer, 2 badtoken, 3 payload
; on ok sets auth_uid, auth_unl, unamebuf
auth:
  push rbx
  push r12
  push r13
  push r14
  push r15
%define A_PL 0
%define A_HDR 2048
%define A_SIG 2560
%define A_MAC 2592
%define A_D2 2624
%define A_SCR 2640
%define A_JSP 2680
%define A_JSE 2688
%define A_FRAME 2704
  sub rsp, A_FRAME
  mov r12, rdi               ; h
  mov r13, rsi               ; hl
  cmp r13, 7
  jb .nobearer
  lea rsi, [bearer]
  mov rdi, r12
  mov rcx, 7
  call memeq_n
  jne .nobearer
  lea r14, [r12+7]           ; t
  lea r15, [r12+r13]         ; te
  mov rdi, r14
  mov rsi, r15
  mov dl, '.'
  call memchr_
  test rax, rax
  jz .bad
  mov rbx, rax               ; d1
  lea rdi, [rbx+1]
  mov rsi, r15
  mov dl, '.'
  call memchr_
  test rax, rax
  jz .bad
  mov [rsp+A_D2], rax        ; d2
  lea rdi, [rax+1]
  mov rsi, r15
  mov dl, '.'
  call memchr_
  test rax, rax
  jnz .bad
  mov r11, [rsp+A_D2]
  mov rax, r15
  sub rax, r11
  dec rax
  cmp rax, 43
  jne .bad
  lea rdi, [r11+1]
  mov rsi, 43
  lea rdx, [rsp+A_SIG]
  call b64dec
  cmp rax, 32
  jne .bad
  mov r11, [rsp+A_D2]
  mov rsi, r14
  mov rdx, r11
  sub rdx, r14
  lea rdi, [rsp+A_MAC]
  call hmac
  xor edx, edx
  xor rcx, rcx
.cmp:
  mov al, [rsp+A_SIG+rcx]
  xor al, [rsp+A_MAC+rcx]
  or dl, al
  inc rcx
  cmp rcx, 32
  jb .cmp
  test dl, dl
  jnz .bad
  mov rax, rbx
  sub rax, r14
  cmp rax, 680
  ja .bad
  mov r11, [rsp+A_D2]
  mov rax, r11
  sub rax, rbx
  dec rax
  cmp rax, 2700
  ja .bad
  ; hn = b64dec(t, d1-t, hdr)
  mov rdi, r14
  mov rsi, rbx
  sub rsi, r14
  lea rdx, [rsp+A_HDR]
  call b64dec
  test rax, rax
  js .bad
  mov r13, rax               ; hn
  ; pn = b64dec(d1+1, d2-d1-1, pl)
  lea rdi, [rbx+1]
  mov r11, [rsp+A_D2]
  mov rsi, r11
  sub rsi, rbx
  dec rsi
  lea rdx, [rsp+A_PL]
  call b64dec
  test rax, rax
  js .bad
  mov r12, rax               ; pn
  ; jdoc(hdr, hn, w_alg, 1)
  lea rdi, [rsp+A_HDR]
  mov rsi, r13
  lea rdx, [w_alg]
  mov ecx, 1
  call jdoc
  test rax, rax
  jz .bad
  mov rax, [w_alg+16]
  test rax, rax
  jz .bad
  cmp byte [rax], '"'
  jne .bad
  mov [rsp+A_JSP], rax
  mov rcx, [w_alg+24]
  mov [rsp+A_JSE], rcx
  lea rdi, [rsp+A_JSP]
  lea rsi, [rsp+A_SCR]
  mov rdx, 8
  call jstr
  cmp rax, 5
  jne .bad
  lea rdi, [rsp+A_SCR]
  lea rsi, [v_hs256]
  mov rcx, 5
  call memeq_n
  jne .bad
  ; jdoc(pl, pn, w_pl, 4)
  lea rdi, [rsp+A_PL]
  mov rsi, r12
  lea rdx, [w_pl]
  mov ecx, 4
  call jdoc
  test rax, rax
  jz .bad
  mov rax, [w_pl+16]
  test rax, rax
  jz .bad  ; exp is required
  mov rdi, rax               ; exp / nbf must be numbers, checked against now
  mov rsi, [w_pl+24]
  call jnum
  test eax, eax
  jz .bad
  cvtsi2sd xmm1, qword [now_s]
  ucomisd xmm1, xmm0
  jae .bad                   ; now >= exp
  mov rax, [w_pl+48]
  test rax, rax
  jz .no_nbf
  mov rdi, rax
  mov rsi, [w_pl+56]
  call jnum
  test eax, eax
  jz .bad
  cvtsi2sd xmm1, qword [now_s]
  ucomisd xmm1, xmm0
  jb .bad                    ; now < nbf
.no_nbf:
  mov rax, [w_pl+80]
  test rax, rax
  jz .payload
  cmp byte [rax], '"'
  jne .payload
  mov rax, [w_pl+112]
  test rax, rax
  jz .payload
  cmp byte [rax], '"'
  jne .payload
  ; decode sub -> scratch
  mov rax, [w_pl+80]
  mov [rsp+A_JSP], rax
  mov rax, [w_pl+88]
  mov [rsp+A_JSE], rax
  lea rdi, [rsp+A_JSP]
  lea rsi, [rsp+A_SCR]
  mov rdx, 24
  call jstr
  cmp rax, 0
  jle .payload
  cmp rax, 15
  jg .payload
  mov rcx, rax
  xor r8, r8
  xor r9, r9
.subdig:
  movzx edx, byte [rsp+A_SCR+r9]
  cmp dl, '0'
  jb .payload
  cmp dl, '9'
  ja .payload
  imul r8, r8, 10
  sub dl, '0'
  movzx edx, dl
  add r8, rdx
  inc r9
  cmp r9, rcx
  jb .subdig
  test r8, r8
  jle .payload
  mov [auth_uid], r8
  ; username -> unamebuf
  mov rax, [w_pl+112]
  mov [rsp+A_JSP], rax
  mov rax, [w_pl+120]
  mov [rsp+A_JSE], rax
  lea rdi, [rsp+A_JSP]
  lea rsi, [unamebuf]
  mov rdx, 1024
  call jstr
  cmp rax, 0
  jl .payload
  mov [auth_unl], rax
  xor eax, eax
  jmp .ret
.nobearer:
  mov eax, 1
  jmp .ret
.bad:
  mov eax, 2
  jmp .ret
.payload:
  mov eax, 3
.ret:
  add rsp, A_FRAME
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; jnum: rdi=value start, rsi=end -> eax 1 and xmm0 = the value (strtod, as the C entry), or 0 when it doesn't
; start like a number or is 64 bytes or longer
jnum:
  movzx eax, byte [rdi]
  cmp al, '-'
  je .num
  sub eax, '0'
  cmp eax, 9
  ja .no
.num:
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 64
  jae .no
  push rbx
  lea rbx, [numbuf]
  mov rsi, rdi
  mov rdi, rbx
  rep movsb
  mov byte [rdi], 0
  mov rdi, rbx
  xor esi, esi
  call strtod
  pop rbx
  mov eax, 1
  ret
.no:
  xor eax, eax
  ret

; memchr_: rdi=p, rsi=e, dl=char -> rax=ptr or 0
memchr_:
  cmp rdi, rsi
  jae .no
  cmp [rdi], dl
  je .yes
  inc rdi
  jmp memchr_
.yes:
  mov rax, rdi
  ret
.no:
  xor rax, rax
  ret

; col_text: rdi=stmt, esi=column -> rsi=text, rdx=byte length
col_text:
  push r12
  push r13
  push r14
  mov r12, rdi
  mov r13d, esi
  call sqlite3_column_text
  mov r14, rax
  mov rdi, r12
  mov esi, r13d
  call sqlite3_column_bytes
  movsxd rdx, eax
  mov rsi, r14
  pop r14
  pop r13
  pop r12
  ret

; put_post_row: rdi=stmt, rsi=like count, or <0 to take it from column 4
put_post_row:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, rsi
  PUTL j_id
  mov rdi, rbx
  xor esi, esi
  call sqlite3_column_int64
  mov rdi, rax
  call a_u64
  PUTL j_body
  mov rdi, rbx
  mov esi, 1
  call col_text
  call a_jstr
  PUTL j_created
  mov rdi, rbx
  mov esi, 2
  call col_text
  call a_emit
  PUTL j_author
  mov rdi, rbx
  mov esi, 3
  call col_text
  call a_jstr
  PUTL j_likec
  mov rdi, r12
  test r12, r12
  jns .likes
  mov rdi, rbx
  mov esi, 4
  call sqlite3_column_int64
  mov rdi, rax
.likes:
  call a_u64
  PUTL j_closeobj
  pop r13
  pop r12
  pop rbx
  ret
section .rodata
; serial type -> content bytes, for types 0..11 (10 and 11 are reserved and rejected before this lookup)
small_sizes: db 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0

section .bss
alignb 8
fb_ids:   resq FEED_N
fb_ca:    resb FEED_N*32           ; created_at of each feed entry, copied out of the index cell
fb_cal:   resd FEED_N
fb_likes: resd FEED_N
fb_ord:   resd FEED_N
fb_at:    resd FEED_N
fb_len:   resd FEED_N
fb_span:  resd FEED_SPAN
fb_n:     resd 1
fb_close: resd 1
fb_cols:  resb 4*16                ; col_t {type d, len d, data q}
fb_ucols: resb 2*16
fr_tmp:   resb BATCH_OUT
section .text

; ---------- records: SQLite's own format, decoded here; anything unexpected falls back to SQL ----------
; varint: rsi=p -> rax=value, rdx=bytes. clobbers rcx, r8
varint:
  xor eax, eax
  xor edx, edx
.l:
  movzx ecx, byte [rsi+rdx]
  inc edx
  cmp edx, 9
  je .ninth
  shl rax, 7
  mov r8d, ecx
  and r8d, 0x7f
  or rax, r8
  test cl, 0x80
  jnz .l
  ret
.ninth:
  shl rax, 8
  or rax, rcx
  ret

; rec_parse: rdi=record, rsi=end, rdx=out col_t[], ecx=columns -> eax 1, or 0 on a record this won't decode
rec_parse:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, rdi
  mov r13, rsi
  mov r14, rdx
  mov r15d, ecx
  mov rsi, r12
  call varint
  mov rbx, rax              ; header size
  mov r9, rdx               ; i
  mov rax, r13
  sub rax, r12
  cmp rbx, rax
  ja .bad
  lea r10, [r12+rbx]        ; data
.col:
  test r15d, r15d
  jz .ok
  cmp r9, rbx
  jae .bad
  lea rsi, [r12+r9]
  call varint
  add r9, rdx
  cmp rax, 10
  je .bad
  cmp rax, 11
  je .bad
  cmp rax, 12
  jb .small
  lea rcx, [rax-12]
  shr rcx, 1
  jmp .len
.small:
  lea rcx, [small_sizes]
  movzx ecx, byte [rcx+rax]
.len:
  mov rdx, r13
  sub rdx, r10
  cmp rcx, rdx
  ja .bad
  mov [r14], eax
  mov [r14+4], ecx
  mov [r14+8], r10
  add r10, rcx
  add r14, 16
  dec r15d
  jmp .col
.ok:
  mov eax, 1
  jmp .ret
.bad:
  xor eax, eax
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; col_int: rdi=col -> eax 1 and rdx=value, or 0 if the column is not an integer. clobbers rcx, r8, r9
col_int:
  mov eax, [rdi]
  cmp eax, 8
  je .k0
  cmp eax, 9
  je .k1
  dec eax
  cmp eax, 5
  ja .no
  mov r8, [rdi+8]
  mov ecx, [rdi+4]
  movsx rdx, byte [r8]
  mov r9d, 1
.l:
  cmp r9d, ecx
  jae .yes
  shl rdx, 8
  movzx eax, byte [r8+r9]
  or rdx, rax
  inc r9d
  jmp .l
.yes:
  mov eax, 1
  ret
.k0:
  xor edx, edx
  mov eax, 1
  ret
.k1:
  mov edx, 1
  mov eax, 1
  ret
.no:
  xor eax, eax
  ret

; is_text: rdi=col -> ZF clear when it holds text (serial type >= 13, odd)
%macro IS_TEXT 2              ; %1 = col_t address, %2 = label to jump to when it is not text
  mov eax, [%1]
  cmp eax, 13
  jb %2
  test al, 1
  jz %2
%endmacro

; rec_cols: edi=cursor, rsi=out, edx=columns -> eax 1/0 (0 also when the record spills to overflow pages)
rec_cols:
  push rbx
  push r12
  push r13
  sub rsp, 16
  mov r12, rsi
  mov r13d, edx
  lea rsi, [rsp]
  call td_rec
  test rax, rax
  jz .no
  mov rdi, rax
  mov esi, [rsp]
  add rsi, rax
  mov rdx, r12
  mov ecx, r13d
  call rec_parse
  jmp .ret
.no:
  xor eax, eax
.ret:
  add rsp, 16
  pop r13
  pop r12
  pop rbx
  ret

; row_author: rdi=user id -> eax 1 found (name in fb_ucols+16), 0 use SQL, -1 error. The name stays valid
; while the users cursor doesn't move.
row_author:
  sub rsp, 8
  mov rsi, rdi
  mov edi, 1
  call td_seek
  test eax, eax
  jle .ret
  mov edi, 1
  lea rsi, [fb_ucols]
  mov edx, 2
  call rec_cols
  test eax, eax
  jz .ret
  IS_TEXT fb_ucols+16, .zero
  mov eax, 1
.ret:
  add rsp, 8
  ret
.zero:
  xor eax, eax
  jmp .ret

; put_row: rdi=id, rsi=body col, rdx=created_at, ecx=its length, r8=author col, r9d=like count
put_row:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov r14d, ecx
  mov r15, r8
  mov ebp, r9d
  PUTL j_id
  mov rdi, rbx
  call a_u64
  PUTL j_body
  mov rsi, [r12+8]
  mov edx, [r12+4]
  call a_jstr
  PUTL j_created
  mov rsi, r13
  mov edx, r14d
  call a_emit
  PUTL j_author
  mov rsi, [r15+8]
  mov edx, [r15+4]
  call a_jstr
  PUTL j_likec
  mov edi, ebp
  call a_u64
  PUTL j_closeobj
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; count_likes: rdi=lo, rsi=hi, rdx=u32 counts[post - lo] -> eax 1 done, 0 for SQL to answer, -1 on error.
; Walks likes_post_id_idx from lo, a page at a time: every cell of a leaf is tallied here from the shim's view
; of the page, and SQLite moves the cursor to the next one.
%define PV_DATA 0
%define PV_IDX 8
%define PV_END 16
%define PV_MASK 24
%define PV_NCELL 28
%define PV_IX 32
%define PV_LEAF 36
%define PV_CHILD 40
%define PV_MAXL 44
count_likes:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 80               ; [0,48) page view, [48,64) col, [64] cell
  mov r12, rdi
  mov r13, rsi
  mov r14, rdx
  mov rdi, r12
  call td_likes_from
.on:
  cmp eax, 1
  jne .end
  lea rdi, [rsp]
  call td_page
  mov ebx, [rsp+PV_IX]      ; first
  mov r15d, ebx             ; last: on an interior page the entry under the cursor is one key
  cmp dword [rsp+PV_LEAF], 0
  je .cells
  mov r15d, [rsp+PV_NCELL]
  dec r15d
.cells:
  cmp ebx, r15d
  jg .page_done
  mov rax, [rsp+PV_IDX]
  movzx eax, word [rax+rbx*2]
  rol ax, 8                 ; big-endian cell offset
  and eax, [rsp+PV_MASK]
  add rax, [rsp+PV_DATA]
  mov ecx, [rsp+PV_CHILD]
  add rax, rcx
  mov [rsp+64], rax
  mov rsi, rax
  call varint               ; payload size
  mov ecx, [rsp+PV_MAXL]
  cmp rax, rcx
  ja .weird
  mov rdi, [rsp+64]
  add rdi, rdx
  lea rsi, [rdi+rax]
  cmp rsi, [rsp+PV_END]
  ja .weird
  lea rdx, [rsp+48]
  mov ecx, 1
  call rec_parse
  test eax, eax
  jz .weird
  lea rdi, [rsp+48]
  call col_int
  test eax, eax
  jz .weird
  cmp rdx, r13
  jg .done                  ; past hi
  cmp rdx, r12
  jl .next
  sub rdx, r12
  inc dword [r14+rdx*4]
.next:
  inc ebx
  jmp .cells
.page_done:
  mov edi, r15d
  call td_likes_setix
  mov edi, 3
  call td_next
  jmp .on
.end:
  test eax, eax
  jnz .ret                  ; -1
.done:
  mov eax, 1
  jmp .ret
.weird:
  xor eax, eax
.ret:
  add rsp, 80
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; feed_btree: edi=close -> eax 1 feed written, 0 use SQL, -1 database error.
; The 20 newest off posts_created_at_id_idx, then the rows in ascending id order (consecutive ids are one step
; along a leaf), then one likes walk when the ids are within FEED_SPAN, else one per post.
feed_btree:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov [fb_close], edi
  cmp dword [td_bt_ok], 0
  je .zero
  xor ebx, ebx              ; n
  mov edi, 2
  call td_first
  test eax, eax
  js .ret
  jz .collected
.row:
  mov edi, 2
  lea rsi, [fb_cols]
  mov edx, 3
  call rec_cols
  test eax, eax
  jz .zero
  IS_TEXT fb_cols, .zero
  cmp dword [fb_cols+4], 32
  ja .zero
  lea rdi, [fb_cols+16]
  call col_int
  test eax, eax
  jz .zero
  mov [fb_ids+rbx*8], rdx
  mov ecx, [fb_cols+4]
  mov [fb_cal+rbx*4], ecx
  mov rsi, [fb_cols+8]
  mov rdi, rbx
  shl rdi, 5
  add rdi, fb_ca
  rep movsb
  inc ebx
  cmp ebx, FEED_N
  je .collected
  mov edi, 2
  call td_next
  test eax, eax
  js .ret
  jnz .row
.collected:
  mov [fb_n], ebx
  xor ecx, ecx              ; newest first is normally descending id, so this is usually just a reversal
.sort:
  cmp ecx, ebx
  jae .sorted
  mov edx, ecx              ; j
  mov r8d, ebx
  sub r8d, ecx
  dec r8d                   ; v = n - 1 - i
  mov r9, [fb_ids+r8*8]
.ins:
  test edx, edx
  jz .place
  mov eax, [fb_ord+rdx*4-4]
  cmp [fb_ids+rax*8], r9
  jle .place
  mov [fb_ord+rdx*4], eax
  dec edx
  jmp .ins
.place:
  mov [fb_ord+rdx*4], r8d
  inc ecx
  jmp .sort
.sorted:
  lea rdi, [fb_likes]
  mov ecx, FEED_N
  xor eax, eax
  rep stosd
  test ebx, ebx
  jz .write
  mov eax, [fb_ord]
  mov r12, [fb_ids+rax*8]   ; lo
  lea eax, [rbx-1]
  mov eax, [fb_ord+rax*4]
  mov r13, [fb_ids+rax*8]   ; hi
  mov rax, r13
  sub rax, r12
  cmp rax, FEED_SPAN
  jge .spread
  lea rdi, [fb_span]
  lea rcx, [rax+1]
  xor eax, eax
  rep stosd
  mov rdi, r12
  mov rsi, r13
  lea rdx, [fb_span]
  call count_likes
  test eax, eax
  jle .ret
  xor ecx, ecx
.copy:
  cmp ecx, ebx
  jae .write
  mov rax, [fb_ids+rcx*8]
  sub rax, r12
  mov eax, [fb_span+rax*4]
  mov [fb_likes+rcx*4], eax
  inc ecx
  jmp .copy
.spread:
  xor r14d, r14d
.sp:
  cmp r14d, ebx
  jae .write
  mov rdi, [fb_ids+r14*8]
  mov rsi, rdi
  lea rdx, [fb_likes+r14*4]
  call count_likes
  test eax, eax
  jle .ret
  inc r14d
  jmp .sp
.write:
  mov r15, [awr]            ; mark
  mov edi, 200
  mov esi, [fb_close]
  call begin_resp
  PUTL j_postsarr
  mov r14, [awr]            ; rows start
  xor r12d, r12d            ; k
.wrow:
  cmp r12d, ebx
  jae .reorder
  mov r13d, [fb_ord+r12*4]  ; i
  xor edi, edi
  mov rsi, [fb_ids+r13*8]
  call td_seek
  test eax, eax
  js .err_mark
  jz .zero_mark
  xor edi, edi
  lea rsi, [fb_cols]
  mov edx, 3
  call rec_cols
  test eax, eax
  jz .zero_mark
  IS_TEXT fb_cols+32, .zero_mark
  lea rdi, [fb_cols+16]
  call col_int
  test eax, eax
  jz .zero_mark
  mov rdi, rdx
  call row_author
  test eax, eax
  js .err_mark
  jz .zero_mark
  mov rax, [awr]
  sub rax, r14
  mov [fb_at+r13*4], eax
  mov rdi, [fb_ids+r13*8]
  lea rsi, [fb_cols+32]
  mov rdx, r13
  shl rdx, 5
  add rdx, fb_ca
  mov ecx, [fb_cal+r13*4]
  lea r8, [fb_ucols+16]
  mov r9d, [fb_likes+r13*4]
  call put_row
  mov rax, [awr]
  sub rax, r14
  sub eax, [fb_at+r13*4]
  mov [fb_len+r13*4], eax
  inc r12d
  jmp .wrow
.reorder:                   ; back into feed order
  mov rcx, [awr]
  sub rcx, r14
  mov rsi, r14
  lea rdi, [fr_tmp]
  rep movsb
  mov [awr], r14
  xor r12d, r12d
.ro:
  cmp r12d, ebx
  jae .close
  test r12d, r12d
  jz .nocomma
  mov dil, ','
  call a_char
.nocomma:
  mov eax, [fb_at+r12*4]
  lea rsi, [fr_tmp+rax]
  mov edx, [fb_len+r12*4]
  call a_emit
  inc r12d
  jmp .ro
.close:
  PUTL j_closearr
  call finish
  mov eax, 1
  jmp .ret
.zero_mark:
  mov [awr], r15
.zero:
  xor eax, eax
  jmp .ret
.err_mark:
  mov [awr], r15
  mov eax, -1
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; post_btree: rdi=id, esi=close -> eax 1 written, 0 use SQL (which also answers a missing post), -1 error
post_btree:
  push rbx
  push r12
  push r13
  xor eax, eax
  cmp dword [td_bt_ok], 0
  je .ret
  mov rbx, rdi
  mov r12d, esi
  xor edi, edi
  mov rsi, rbx
  call td_seek
  test eax, eax
  jle .ret
  xor edi, edi
  lea rsi, [fb_cols]
  mov edx, 4
  call rec_cols
  test eax, eax
  jz .ret
  IS_TEXT fb_cols+32, .zero
  IS_TEXT fb_cols+48, .zero
  lea rdi, [fb_cols+16]
  call col_int
  test eax, eax
  jz .ret
  mov rdi, rdx
  call row_author
  test eax, eax
  jle .ret
  mov dword [fb_likes], 0
  mov rdi, rbx
  mov rsi, rbx
  lea rdx, [fb_likes]
  call count_likes
  test eax, eax
  jle .ret
  mov edi, 200
  mov esi, r12d
  call begin_resp
  PUTL j_postobj
  mov rdi, rbx
  lea rsi, [fb_cols+32]
  mov rdx, [fb_cols+56]
  mov ecx, [fb_cols+52]
  lea r8, [fb_ucols+16]
  mov r9d, [fb_likes]
  call put_row
  PUTL j_closeobj
  call finish
  mov eax, 1
  jmp .ret
.zero:
  xor eax, eax
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; step_reset: rdi=stmt -> eax = 0 if the step succeeded, else its error code (sqlite3_reset's result)
step_reset:
  push rbx
  mov rbx, rdi
  call sqlite3_step
  mov rdi, rbx
  call sqlite3_reset
  pop rbx
  ret

; txn: opens the batch transaction (SQL BEGIN, then the shim's B-tree read side) on first use -> eax 1/0
txn:
  cmp dword [in_txn], 0
  jne .yes
  sub rsp, 8
  mov rdi, [st_begin]
  call step_reset
  test eax, eax
  jnz .no
  call td_begin
  test eax, eax
  jnz .ok
  mov rdi, [st_rollback]
  call step_reset
.no:
  add rsp, 8
  xor eax, eax
  ret
.ok:
  add rsp, 8
  mov dword [in_txn], 1
.yes:
  mov eax, 1
  ret

; wstep: rdi=stmt -> eax=rc. a write inside the batch transaction
wstep:
  sub rsp, 8
  call sqlite3_step
  cmp eax, SQLITE_ROW
  je .w
  cmp eax, SQLITE_DONE
  jne .r
.w:
  mov dword [txn_writes], 1
.r:
  add rsp, 8
  ret

; db_fail: a failed statement may have rolled the whole batch back
db_fail:
  cmp dword [in_txn], 0
  je .r
  sub rsp, 8
  mov rdi, [td_db]
  call sqlite3_get_autocommit
  test eax, eax
  jz .r8
  call td_end
  mov dword [in_txn], 0
  cmp dword [txn_writes], 0
  je .nw
  mov dword [batch_failed], 1
.nw:
  mov dword [txn_writes], 0
.r8:
  add rsp, 8
.r:
  ret

; ---------- handle ----------
; uses globals g_*. r15b = close flag throughout.
handle:
  push rbx
  push r12
  push r13
  push r14
  push r15
  movzx r15d, byte [g_close]
  ; strip query
  mov rdi, [g_path]
  lea rsi, [rdi]
  add rsi, [g_pl]
  mov dl, '?'
  call memchr_
  test rax, rax
  jz .noq
  mov rcx, rax
  sub rcx, [g_path]
  mov [g_pl], rcx
.noq:
  mov byte [.is_get], 0
  mov byte [.is_post], 0
  mov rsi, [g_m]
  cmp qword [g_ml], 3
  jne .notget
  cmp word [rsi], 'GE'
  jne .notget
  cmp byte [rsi+2], 'T'
  jne .notget
  mov byte [.is_get], 1
.notget:
  cmp qword [g_ml], 4
  jne .notpost
  cmp dword [rsi], 'POST'
  jne .notpost
  mov byte [.is_post], 1
.notpost:
  mov rbx, [g_path]
  mov r12, [g_pl]
  ; GET /feed
  cmp byte [.is_get], 0
  je .chk_posts7
  cmp r12, 5
  jne .chk_posts7
  mov rdi, rbx
  lea rsi, [m_feed]
  mov rcx, 5
  call memeq_n
  jnz .chk_posts7
  call txn
  test rax, rax
  jz .e500
  movzx edi, r15b
  call feed_btree
  test eax, eax
  jz .feed_sql
  jns .ret
  call db_fail
  jmp .e500
.feed_sql:
  mov r13, [awr]             ; mark
  mov edi, 200
  movzx esi, r15b
  call begin_resp
  PUTL j_postsarr
  xor r14, r14              ; i
.feedloop:
  mov rdi, [st_feed]
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .feeddone
  test r14, r14
  jz .nocomma
  mov dil, ','
  call a_char
.nocomma:
  inc r14
  mov rdi, [st_feed]
  mov rsi, -1
  call put_post_row
  jmp .feedloop
.feeddone:
  mov rdi, [st_feed]
  call sqlite3_reset
  test eax, eax
  jnz .feed_err
  PUTL j_closearr
  call finish
  jmp .ret
.feed_err:
  call db_fail
  mov [awr], r13
  jmp .e500
.chk_posts7:
  cmp r12, 7
  jbe .chk_posts_exact
  mov rdi, rbx
  lea rsi, [m_posts7]
  mov rcx, 7
  call memeq_n
  jnz .chk_posts_exact
  lea r13, [rbx+7]          ; seg
  mov rdi, r13
  lea rsi, [rbx+r12]
  mov dl, '/'
  call memchr_
  mov r14, rax              ; sl (0 if none)
  ; GET /posts/:id
  cmp byte [.is_get], 0
  je .chk_like
  test r14, r14
  jnz .chk_like
  mov rdi, r13
  lea rsi, [rbx+r12]
  sub rsi, r13
  call parse_id
  cmp rax, 0
  jl .e_badid
  mov [.idv], rax
  call txn
  test rax, rax
  jz .e500
  mov rdi, [.idv]
  movzx esi, r15b
  call post_btree
  test eax, eax
  jz .post_sql
  jns .ret
  call db_fail
  jmp .e500
.post_sql:
  mov rdi, [st_post]
  mov esi, 1
  mov rdx, [.idv]
  call sqlite3_bind_int64
  mov r13, [awr]            ; mark
  mov rdi, [st_post]
  call sqlite3_step
  mov [.rc], eax
  cmp eax, SQLITE_ROW
  jne .post_notrow
  mov edi, 200
  movzx esi, r15b
  call begin_resp
  PUTL j_postobj
  mov rdi, [st_post]
  mov rsi, -1
  call put_post_row
  PUTL j_closeobj
  call finish
  jmp .post_reset
.post_notrow:
  cmp dword [.rc], SQLITE_DONE
  jne .post_500
  mov edi, 404
  lea rsi, [e_nopost]
  mov edx, e_nopost_len
  movzx ecx, r15b
  call send_err
  jmp .post_reset
.post_500:
  call db_fail
  mov [awr], r13
  mov edi, 500
  lea rsi, [e_500]
  mov edx, e_500_len
  movzx ecx, r15b
  call send_err
.post_reset:
  mov rdi, [st_post]
  call sqlite3_reset
  jmp .ret
.chk_like:
  ; POST /posts/:id/like
  cmp byte [.is_post], 0
  je .chk_posts_exact
  test r14, r14
  jz .chk_posts_exact
  lea rax, [rbx+r12]
  sub rax, r14
  cmp rax, 5
  jne .chk_posts_exact
  mov rdi, r14
  lea rsi, [m_like]
  mov rcx, 5
  call memeq_n
  jnz .chk_posts_exact
  mov rdi, [g_authh]        ; auth first
  mov rsi, [g_al]
  call auth
  test eax, eax
  jnz .auth_err
  mov rdi, r13
  mov rsi, r14
  sub rsi, r13
  call parse_id
  cmp rax, 0
  jl .e_badid
  mov [.idv], rax
  call txn
  test rax, rax
  jz .e500
  cmp dword [td_bt_ok], 0   ; without B-tree reads the statement's own EXISTS decides
  je .like_ins
  xor edi, edi
  mov rsi, [.idv]
  call td_seek
  test eax, eax
  js .like_500
  jnz .like_ins
  ERR 404, e_nopost
.like_ins:
  mov rdi, [st_like]
  mov esi, 1
  mov rdx, [auth_uid]
  call sqlite3_bind_int64
  mov rdi, [st_like]
  mov esi, 2
  mov rdx, [.idv]
  call sqlite3_bind_int64
  mov rdi, [st_like]
  call wstep
  mov [.rc], eax
  mov rdi, [st_like]
  call sqlite3_reset
  cmp dword [.rc], SQLITE_DONE
  jne .like_500
  mov rdi, [td_db]
  call sqlite3_changes
  mov [.created], eax
  test eax, eax
  jnz .like_resp
  cmp dword [td_bt_ok], 0
  jne .like_resp
  mov rdi, [st_post]
  mov esi, 1
  mov rdx, [.idv]
  call sqlite3_bind_int64
  mov rdi, [st_post]
  call sqlite3_step
  mov [.rc], eax
  mov rdi, [st_post]
  call sqlite3_reset
  cmp dword [.rc], SQLITE_DONE
  jne .like_exists_ok
  ERR 404, e_nopost
.like_exists_ok:
  cmp dword [.rc], SQLITE_ROW
  jne .like_500
.like_resp:
  mov edi, 201
  cmp dword [.created], 0
  jne .l201
  mov edi, 200
.l201:
  movzx esi, r15b
  call begin_resp
  PUTL j_liked
  cmp dword [.created], 0
  je .lfalse
  PUTL j_false
  jmp .lpid
.lfalse:
  PUTL j_true
.lpid:
  PUTL j_postid
  mov rdi, [.idv]
  call a_u64
  PUTL j_closeobj
  call finish
  jmp .ret
.like_500:
  call db_fail
  jmp .e500
.chk_posts_exact:
  ; POST /posts
  cmp byte [.is_post], 0
  je .chk_health
  cmp r12, 6
  jne .chk_health
  mov rdi, rbx
  lea rsi, [m_posts]
  mov rcx, 6
  call memeq_n
  jnz .chk_health
  mov rdi, [g_authh]
  mov rsi, [g_al]
  call auth
  test eax, eax
  jnz .auth_err
  mov rdi, [g_body]
  mov rsi, [g_bl]
  lea rdx, [w_body]
  mov ecx, 1
  call jdoc
  test rax, rax
  jz .e_badjson
  mov rax, [w_body+16]
  test rax, rax
  jz .e_bodyreq
  cmp byte [rax], '"'
  jne .e_bodyreq
  mov [.js], rax
  mov rax, [w_body+24]
  mov [.je], rax
  lea rdi, [.js]
  lea rsi, [txtbuf]
  mov rdx, MAXREQ
  call jstr
  cmp rax, 0
  jl .e_badjson
  lea r13, [txtbuf]         ; s
  lea r14, [txtbuf+rax]     ; se
.trimL:
  cmp r13, r14
  jae .trimdone
  mov rdi, r13
  mov rsi, r14
  call jsws_at
  test rax, rax
  jz .trimR
  add r13, rax
  jmp .trimL
.trimR:
  cmp r14, r13
  jbe .trimdone
  mov rdi, r13
  mov rsi, r14
  call jsws_before
  test rax, rax
  jz .trimdone
  sub r14, rax
  jmp .trimR
.trimdone:
  cmp r13, r14
  je .e_bodyreq
  cmp byte [r13], 0         ; length() stops at a NUL, so the CHECK would fail
  je .e_bodyreq
  xor rcx, rcx              ; code points
  mov rsi, r13
.cntloop:
  cmp rsi, r14
  jae .cntdone
  movzx eax, byte [rsi]
  and al, 0xc0
  cmp al, 0x80
  je .nocount
  inc rcx
.nocount:
  inc rsi
  jmp .cntloop
.cntdone:
  cmp rcx, 500
  ja .e_bodylong
  call txn
  test rax, rax
  jz .e500
  mov rdi, [st_ins]
  mov esi, 1
  mov rdx, [auth_uid]
  call sqlite3_bind_int64
  mov [.bodyp], r13
  mov [.bodye], r14
  mov rdi, [st_ins]
  mov esi, 2
  mov rdx, r13
  mov rcx, r14
  sub rcx, r13
  xor r8, r8                ; SQLITE_STATIC
  call sqlite3_bind_text
  mov rdi, [st_ins]
  call wstep
  cmp eax, SQLITE_ROW
  je .ins_ok
  mov rdi, [st_ins]
  call sqlite3_reset
  call db_fail
  jmp .e500
.ins_ok:
  mov r13, [awr]            ; mark
  mov edi, 201
  movzx esi, r15b
  call begin_resp
  PUTL j_newpost
  mov rdi, [st_ins]
  xor esi, esi
  call sqlite3_column_int64
  mov rdi, rax
  call a_u64
  PUTL j_body
  mov rsi, [.bodyp]
  mov rdx, [.bodye]
  sub rdx, rsi
  call a_jstr
  PUTL j_created
  mov rdi, [st_ins]
  mov esi, 1
  call col_text
  call a_emit
  PUTL j_author
  lea rsi, [unamebuf]
  mov rdx, [auth_unl]
  call a_jstr
  PUTL j_np_like0
  call finish
  mov rdi, [st_ins]         ; run the INSERT ... RETURNING to completion so the insert finishes
  call step_reset
  test eax, eax
  jz .ret
  call db_fail
  mov [awr], r13
  jmp .e500
.chk_health:
  cmp byte [.is_get], 0
  je .e404
  cmp r12, 7
  jne .e404
  mov rdi, rbx
  lea rsi, [m_health]
  mov rcx, 7
  call memeq_n
  jnz .e404
  mov rdi, [st_one]
  call sqlite3_step
  mov [.rc], eax
  mov rdi, [st_one]
  call sqlite3_reset
  cmp dword [.rc], SQLITE_ROW
  jne .health_bad
  mov edi, 200
  movzx esi, r15b
  call begin_resp
  PUTL j_health_ok
  mov rdi, [now_s]
  sub rdi, [started]
  call a_u64
  PUTL j_closeobj
  call finish
  jmp .ret
.health_bad:
  mov edi, 503
  movzx esi, r15b
  call begin_resp
  PUTL j_health_bad
  mov rdi, [td_db]
  call sqlite3_errmsg
  mov rsi, rax
  xor rdx, rdx
.el:
  cmp byte [rsi+rdx], 0
  je .eld
  inc rdx
  jmp .el
.eld:
  call a_jstr
  PUTL j_qemark
  call finish
  jmp .ret
.auth_err:
  cmp eax, 1
  je .ae1
  cmp eax, 3
  je .ae3
  ERR 401, e_badtoken
.ae1:
  ERR 401, e_nobearer
.ae3:
  ERR 401, e_payload
.e_badid:
  ERR 400, e_badid
.e_badjson:
  ERR 400, e_badjson
.e_bodyreq:
  ERR 400, e_bodyreq
.e_bodylong:
  ERR 400, e_bodylong
.e404:
  ERR 404, e_notfound
.e500:
  ERR 500, e_500
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

section .bss
.is_get: resb 1
.is_post: resb 1
alignb 8
.idv: resq 1
.rc: resd 1
.created: resd 1
.js: resq 1
.je: resq 1
.bodyp: resq 1
.bodye: resq 1
section .text

; memeq_n: rdi=a, rsi=b, rcx=len -> ZF=1 if equal (exact). clobbers rax
memeq_n:
  test rcx, rcx
  jz .eq
.l:
  mov al, [rdi]
  cmp al, [rsi]
  jne .ne
  inc rdi
  inc rsi
  dec rcx
  jnz .l
.eq:
  xor eax, eax
  ret
.ne:
  or eax, 1
  ret

; memmem_crlf2: rdi=p, rsi=end, looks for CR LF CR LF -> rax=ptr to first CR or 0
memmem_crlf2:
  lea rcx, [rsi-3]
.l:
  cmp rdi, rcx
  jae .no
  cmp dword [rdi], 0x0a0d0a0d   ; little-endian "\r\n\r\n"
  je .yes
  inc rdi
  jmp .l
.yes:
  mov rax, rdi
  ret
.no:
  xor rax, rax
  ret

; serve: rsi=buf, rdx=n -> rax=used or -1 ; sets closing_flag, arena_full, hold_hit
; stack locals: [8] he [16] le [24] sp1 [32] sp2 [44] close [48] authh [56] al [64] clen [72] expect [80] body
%define L_SIZE 96
serve:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, L_SIZE
  mov rbx, rsi              ; buf
  mov r12, rdx              ; n
  xor r13, r13              ; used
.loop:
  cmp r13, r12
  jae .done
  cmp dword [closing_flag], 0
  jne .done
  cmp dword [nsegs], MAXSEG ; the rest waits for the next batch
  jae .full
  mov rax, [awr]
  lea rcx, [arena + (BATCH_OUT - 2*MAXREQ)]
  cmp rax, rcx
  jbe .arena_ok
.full:
  mov dword [arena_full], 1
  jmp .done
.arena_ok:
  lea r14, [rbx+r13]        ; p
  lea r15, [rbx+r12]        ; e
  cmp dword [hold_posts], 0 ; a POST waits, unread, while the last of the log is copied back
  je .nohold
  cmp byte [r14], 'P'
  jne .nohold
  mov dword [hold_hit], 1
  jmp .done
.nohold:
  mov rdi, r14
  mov rsi, r15
  call memmem_crlf2
  test rax, rax
  jnz .havehdr
  mov rax, r15
  sub rax, r14
  cmp rax, MAXREQ
  ja .fail
  jmp .done
.havehdr:
  mov [rsp+8], rax          ; he (first \r of the \r\n\r\n)
  mov rdi, r14
  lea rsi, [rax+2]
  mov dl, 13
  call memchr_
  mov [rsp+16], rax         ; le
  mov rdi, r14
  mov rsi, rax
  mov dl, ' '
  call memchr_
  test rax, rax
  jz .fail
  mov [rsp+24], rax         ; sp1
  lea rdi, [rax+1]
  mov rsi, [rsp+16]
  mov dl, ' '
  call memchr_
  test rax, rax
  jz .fail
  mov [rsp+32], rax         ; sp2
  mov rcx, [rsp+16]
  sub rcx, rax
  dec rcx                   ; le-sp2-1
  mov dword [rsp+44], 0     ; close
  cmp rcx, 8
  jne .notv10
  mov rcx, [rax+1]
  mov rdx, 0x302e312f50545448    ; "HTTP/1.0"
  cmp rcx, rdx
  jne .notv10
  mov dword [rsp+44], 1
.notv10:
  mov qword [rsp+48], 0     ; authh
  mov qword [rsp+56], 0     ; al
  mov qword [rsp+64], 0     ; clen
  mov dword [rsp+72], 0     ; expect
  mov r14, [rsp+16]
  add r14, 2                ; h
.hloop:
  mov rax, [rsp+8]
  add rax, 2                ; he+2
  cmp r14, rax
  jae .hdone
  mov rdi, r14
  mov rsi, rax
  mov dl, 13
  call memchr_
  test rax, rax
  jz .fail                  ; a bare CR just before the end of the head
  mov r15, rax              ; hl
  mov rdi, r14
  mov rsi, r15
  mov dl, ':'
  call memchr_
  test rax, rax
  jz .hnext
  mov r8, rax               ; c
  lea r9, [r8+1]            ; v
.skipv:
  cmp r9, r15
  jae .vdone
  movzx eax, byte [r9]
  cmp al, ' '
  je .skv
  cmp al, 9
  jne .vdone
.skv:
  inc r9
  jmp .skipv
.vdone:
  mov r10, r15              ; ve
.trimve:
  cmp r10, r9
  jbe .vetrimdone
  movzx eax, byte [r10-1]
  cmp al, ' '
  je .vd
  cmp al, 9
  jne .vetrimdone
.vd:
  dec r10
  jmp .trimve
.vetrimdone:
  mov rcx, r8
  sub rcx, r14              ; kn
  cmp rcx, 14
  jne .not_cl
  mov rdi, r14
  lea rsi, [h_contentlength]
  call memeq_ci
  jnz .hnext
  xor rax, rax
  mov rsi, r9
.cldig:
  cmp rsi, r10
  jae .clset
  movzx edx, byte [rsi]
  sub edx, '0'
  cmp edx, 9
  ja .fail
  imul rax, rax, 10
  add rax, rdx
  cmp rax, MAXREQ
  ja .fail
  inc rsi
  jmp .cldig
.clset:
  mov [rsp+64], rax
  jmp .hnext
.not_cl:
  cmp rcx, 13
  jne .not_auth
  mov rdi, r14
  lea rsi, [h_authorization]
  call memeq_ci
  jnz .hnext
  mov [rsp+48], r9
  mov rax, r10
  sub rax, r9
  mov [rsp+56], rax
  jmp .hnext
.not_auth:
  cmp rcx, 10
  jne .not_conn
  mov rdi, r14
  lea rsi, [h_connection]
  call memeq_ci
  jnz .hnext
  mov rax, r10
  sub rax, r9
  cmp rax, 5
  jne .conn_ka
  mov rdi, r9
  lea rsi, [v_close]
  mov rcx, 5
  call memeq_ci
  jnz .hnext
  mov dword [rsp+44], 1
  jmp .hnext
.conn_ka:
  cmp rax, 10
  jne .hnext
  mov rdi, r9
  lea rsi, [v_keepalive]
  mov rcx, 10
  call memeq_ci
  jnz .hnext
  mov dword [rsp+44], 0
  jmp .hnext
.not_conn:
  cmp rcx, 6
  jne .not_expect
  mov rdi, r14
  lea rsi, [h_expect]
  call memeq_ci
  jnz .hnext
  xor eax, eax              ; expect = the value is exactly 100-continue
  mov [rsp+72], eax
  mov rax, r10
  sub rax, r9
  cmp rax, 12
  jne .hnext
  mov rdi, r9
  lea rsi, [v_100continue]
  mov rcx, 12
  call memeq_ci
  jnz .hnext
  mov dword [rsp+72], 1
  jmp .hnext
.not_expect:
  cmp rcx, 17
  jne .hnext
  mov rdi, r14
  lea rsi, [h_te]
  call memeq_ci
  jz .fail                  ; chunked uploads aren't supported
.hnext:
  lea r14, [r15+2]          ; h = hl+2
  jmp .hloop
.hdone:
  mov rax, [rsp+8]
  add rax, 4
  mov [rsp+80], rax         ; body
  lea rcx, [rbx+r12]        ; e
  mov rdx, rcx
  sub rdx, rax
  cmp rdx, [rsp+64]
  jae .complete
  cmp dword [rsp+72], 0     ; incomplete: maybe 100-continue, once per request
  je .done
  cmp rax, rcx
  jne .done
  mov rax, [g_sent100]
  cmp byte [rax], 0
  jne .done
  mov byte [rax], 1
  PUTL s_100cont
  jmp .done
.complete:
  mov rax, [g_sent100]
  mov byte [rax], 0
  lea r8, [rbx+r13]
  mov [g_m], r8
  mov rax, [rsp+24]         ; sp1
  sub rax, r8
  mov [g_ml], rax
  mov rax, [rsp+24]
  inc rax                   ; path
  mov [g_path], rax
  mov rcx, [rsp+32]         ; sp2
  sub rcx, rax
  mov [g_pl], rcx
  mov rax, [rsp+48]
  mov [g_authh], rax
  mov rax, [rsp+56]
  mov [g_al], rax
  mov rax, [rsp+80]
  mov [g_body], rax
  mov rax, [rsp+64]
  mov [g_bl], rax
  mov eax, dword [rsp+44]
  mov [g_close], rax
  call handle
  cmp dword [rsp+44], 0
  je .nocl
  mov dword [closing_flag], 1
.nocl:
  mov rax, [rsp+80]         ; used = body+clen - buf
  add rax, [rsp+64]
  sub rax, rbx
  mov r13, rax
  jmp .loop
.done:
  mov rax, r13
  jmp .ret
.fail:
  mov rax, -1
.ret:
  add rsp, L_SIZE
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; prep: rdi=sql ptr -> rax=stmt
prep:
  push rbx
  sub rsp, 16
  mov rbx, rdi
  mov rdi, [td_db]
  mov rsi, rbx
  mov rdx, -1
  mov ecx, SQLITE_PREP_PERSISTENT
  lea r8, [rsp]
  xor r9, r9
  call sqlite3_prepare_v3
  test eax, eax
  jnz .err
  mov rax, [rsp]
  add rsp, 16
  pop rbx
  ret
.err:
  mov edi, 1
  call exit

; db_open: rdi=path. The shim opens it (and starts the checkpoint thread); the statements are ours.
db_open:
  sub rsp, 8
  call td_open
  lea rdi, [sql_feed]
  call prep
  mov [st_feed], rax
  lea rdi, [sql_post]
  call prep
  mov [st_post], rax
  lea rdi, [sql_ins]
  call prep
  mov [st_ins], rax
  lea rdi, [sql_like_bt]
  cmp dword [td_bt_ok], 0
  jne .like
  lea rdi, [sql_like]
.like:
  call prep
  mov [st_like], rax
  lea rdi, [sql_one]
  call prep
  mov [st_one], rax
  lea rdi, [sql_begin]
  call prep
  mov [st_begin], rax
  lea rdi, [sql_commit]
  call prep
  mov [st_commit], rax
  lea rdi, [sql_rollback]
  call prep
  mov [st_rollback], rax
  add rsp, 8
  ret

; parse_ipv4: rdi=string "a.b.c.d" -> eax addr in memory order (a at sockaddr+4); 0 octets on a bad string
parse_ipv4:
  xor r9, r9               ; octet index
  xor eax, eax             ; current value
  lea r11, [sockaddr+4]
.l:
  movzx edx, byte [rdi]
  test dl, dl
  jz .end
  cmp dl, '.'
  je .dot
  cmp dl, '0'
  jb .next
  cmp dl, '9'
  ja .next
  imul eax, eax, 10
  sub dl, '0'
  movzx edx, dl
  add eax, edx
.next:
  inc rdi
  jmp .l
.dot:
  cmp r9, 3
  jae .next
  mov [r11+r9], al
  inc r9
  xor eax, eax
  inc rdi
  jmp .l
.end:
  mov [r11+r9], al
  mov eax, dword [sockaddr+4]
  ret

; getenv_: rdi=name, rsi=namelen -> rax=value ptr or 0
getenv_:
  push rbx
  push r12
  push r13
  mov r12, rdi
  mov r13, rsi
  mov rbx, [envp_ptr]
.l:
  mov rdi, [rbx]
  test rdi, rdi
  jz .no
  mov rsi, r12
  mov rcx, r13
.cmp:
  test rcx, rcx
  jz .checkeq
  mov al, [rdi]
  cmp al, [rsi]
  jne .nextenv
  inc rdi
  inc rsi
  dec rcx
  jmp .cmp
.checkeq:
  cmp byte [rdi], '='
  jne .nextenv
  lea rax, [rdi+1]
  pop r13
  pop r12
  pop rbx
  ret
.nextenv:
  add rbx, 8
  jmp .l
.no:
  xor rax, rax
  pop r13
  pop r12
  pop rbx
  ret

; ---------- growable lists: {a q, head d, n d, cap d}; a FIFO pops from head ----------
%define V_A 0
%define V_HEAD 8
%define V_N 12
%define V_CAP 16
section .bss
alignb 8
v_deferred: resb 32        ; connections held back by a full arena
v_held:     resb 32        ; connections whose next request is a POST held during the last log copy
v_eofs:     resb 32        ; clients that finished sending: close once answered
v_limbo:    resb 32        ; send buffers of closed connections, freed once their cancels are in
v_readq:    resb 32        ; connections to read (FIFO)
v_readyq:   resb 32        ; connections with bytes read and not yet answered (FIFO)
v_tmp:      resb 32
section .text

; vpush: rdi=list, rsi=value
vpush:
  push rbx
  push r12
  sub rsp, 8
  mov rbx, rdi
  mov r12, rsi
  mov eax, [rbx+V_N]
  cmp eax, [rbx+V_CAP]
  jb .room
  mov esi, [rbx+V_CAP]
  add esi, esi
  mov eax, 1024
  test esi, esi
  cmovz esi, eax
  mov [rbx+V_CAP], esi
  shl rsi, 3
  mov rdi, [rbx+V_A]
  call realloc
  test rax, rax
  jz .oom
  mov [rbx+V_A], rax
.room:
  mov eax, [rbx+V_N]
  mov rcx, [rbx+V_A]
  mov [rcx+rax*8], r12
  inc dword [rbx+V_N]
  add rsp, 8
  pop r12
  pop rbx
  ret
.oom:
  call abort

; fifo_pop: rdi=list (not empty) -> rax=value
fifo_pop:
  mov rcx, [rdi+V_A]
  mov eax, [rdi+V_HEAD]
  mov rdx, [rcx+rax*8]
  inc eax
  mov [rdi+V_HEAD], eax
  cmp eax, [rdi+V_N]
  jne .chk
  mov qword [rdi+V_HEAD], 0 ; head = n = 0
  mov rax, rdx
  ret
.chk:
  cmp eax, 65536
  jb .ret
  lea r8d, [rax+rax]
  cmp r8d, [rdi+V_N]
  jb .ret
  push rdx                  ; compact: n -= head, move the rest to the front
  sub rsp, 16
  mov esi, eax
  mov edx, [rdi+V_N]
  sub edx, eax
  mov [rdi+V_N], edx
  mov dword [rdi+V_HEAD], 0
  shl rdx, 3
  lea rsi, [rcx+rsi*8]
  mov rdi, rcx
  call memmove
  add rsp, 16
  pop rax
  ret
.ret:
  mov rax, rdx
  ret

; vtake: rdi=list -> moves it into v_tmp and empties it (so the walk may push to it again)
vtake:
  movups xmm0, [rdi]
  movups [v_tmp], xmm0
  mov rax, [rdi+16]
  mov [v_tmp+16], rax
  xorps xmm0, xmm0
  movups [rdi], xmm0
  mov qword [rdi+16], 0
  ret

section .bss
alignb 16
rlbuf:    resb 16
params:   resb 120               ; struct io_uring_params
rsrc:     resb 32                ; struct io_uring_rsrc_register / io_uring_rsrc_update
bufreg:   resb 40                ; struct io_uring_buf_reg
nrings:   resd 1
ring_cap: resd 1                 ; rings we may still add (lowered if a setup fails)
tsize:    resd 1
armed:    resd NLISTEN           ; accepts in flight per listener
lfds:     resd NLISTEN
answers_left: resd 1
hold_posts: resd 1
hold_hit:   resd 1
npool:    resd 1
alignb 8
pool:     resq POOL_MAX
g_sent100: resq 1                ; the current connection's "100 Continue already sent" flag
no_conn:  resq 1
wal_msg:  resq 1
wal_outcome: resq 1
wal_busy: resd 1
wal_asked_at: resd 1
wal_last_pass: resd 1
alignb 8
post_deadline: resq 1            ; 0: no limit
kicked_to: resq 1
mono_ts:  resq 2
one64:    resq 1
section .text

; mmap_anon: rdi=size -> rax ptr (or <0 err)
mmap_anon:
  mov rsi, rdi
  xor edi, edi
  mov edx, 3
  mov r10d, 0x22
  mov r8, -1
  xor r9d, r9d
  mov eax, SYS_mmap
  syscall
  ret

; huge_alloc: rdi=size -> rax, 2 MB aligned and MADV_HUGEPAGE (Ubuntu's default THP mode is madvise): a TLB
; miss on a VM walks two page tables. Exits on failure.
huge_alloc:
  push rbx
  push r12
  sub rsp, 8
  add rdi, 0x1fffff
  and rdi, -0x200000
  mov r12, rdi
  add rdi, 0x200000
  call mmap_anon
  cmp rax, -4096
  ja .fail
  add rax, 0x1fffff
  and rax, -0x200000
  mov rbx, rax
  mov rdi, rax
  mov rsi, r12
  mov edx, 14               ; MADV_HUGEPAGE
  mov eax, SYS_madvise
  syscall
  mov rax, rbx
  add rsp, 8
  pop r12
  pop rbx
  ret
.fail:
  mov edi, 1
  call exit

; mono_ns -> rax
mono_ns:
  sub rsp, 8
  mov edi, 1                ; CLOCK_MONOTONIC
  lea rsi, [mono_ts]
  call clock_gettime
  imul rax, [mono_ts], 1000000000
  add rax, [mono_ts+8]
  add rsp, 8
  ret

; ---------- io_uring plumbing (raw syscalls, no liburing) ----------
; ring_enter: rdi=ring, esi=min_complete, edx=flags. Publishes the queued SQEs and submits them.
; ring_enter.arg: same, with r8/r9 = the EXT_ARG pointer and size.
ring_enter:
  xor r8d, r8d
  xor r9d, r9d
.arg:
  mov eax, [rdi+R_TAIL]
  mov rcx, [rdi+R_SQTAIL]
  mov [rcx], eax            ; x86 keeps the SQE stores ordered before this one
  mov rcx, [rdi+R_SQHEAD]
  sub eax, [rcx]
  mov r10d, edx
  or r10d, [rdi+R_EFL]      ; ENTER_REGISTERED_RING when the ring's fd is registered
  mov edx, esi
  mov esi, eax
  mov edi, [rdi+R_EFD]
  mov eax, SYS_io_uring_enter
  syscall
  ret

; get_sqe: edi=ring index -> rax = zeroed SQE (submits first if the queue is full)
get_sqe:
  push rbx
  shl edi, 7
  lea rbx, [rings+rdi]
.retry:
  mov rax, [rbx+R_SQHEAD]
  mov ecx, [rbx+R_TAIL]
  sub ecx, [rax]
  cmp ecx, [rbx+R_SQENT]
  jb .room
  mov rdi, rbx
  xor esi, esi
  xor edx, edx
  call ring_enter
  jmp .retry
.room:
  mov eax, [rbx+R_TAIL]
  lea ecx, [rax+1]
  mov [rbx+R_TAIL], ecx
  and eax, [rbx+R_SQMASK]
  shl rax, 6
  add rax, [rbx+R_SQES]
  xorps xmm0, xmm0
  movups [rax], xmm0
  movups [rax+16], xmm0
  movups [rax+32], xmm0
  movups [rax+48], xmm0
  pop rbx
  ret

; buf_put: rdi=ring, esi=buffer id. Hands a receive buffer back to the kernel.
buf_put:
  mov r8, [rdi+R_BR]
  movzx eax, word [r8+14]   ; the ring's tail lives in bufs[0].resv
  mov ecx, eax
  and ecx, NBUF-1
  shl ecx, 4
  mov edx, esi
  shl rdx, BSZ_SHIFT
  add rdx, [rdi+R_BUFS]
  mov [r8+rcx], rdx
  mov dword [r8+rcx+8], BSZ
  mov [r8+rcx+12], si
  inc eax
  mov [r8+14], ax
  ret

; ring_init: edi=k -> eax 0 ok, -1 failed. Accepted sockets live in each ring's registered file table, not in
; the process descriptor table. A table can't be larger than RLIMIT_NOFILE, so more rings are added as tables
; fill up. DEFER_TASKRUN on every ring: completion work runs when the loop asks, in one batch, never
; interrupting a pass.
ring_init:
  push rbx
  push r12
  push r13
  mov r12d, edi
  shl edi, 7
  lea rbx, [rings+rdi]
  lea rdi, [params]
  xor eax, eax
  mov ecx, 15
  rep stosq
  mov dword [params+4], CQ_ENTRIES
  mov dword [params+8], SETUP_FLAGS
  mov edi, SQ_ENTRIES
  lea rsi, [params]
  mov eax, SYS_io_uring_setup
  syscall
  test eax, eax
  js .fail0
  mov [rbx+R_FD], eax
  mov [rbx+R_EFD], eax
  mov dword [rbx+R_EFL], 0
  ; one mapping holds both rings (IORING_FEAT_SINGLE_MMAP, Linux 5.4+)
  mov eax, [params+P_SQ+24]         ; sq_off.array + sq_entries * 4
  mov ecx, [params]
  lea rax, [rax+rcx*4]
  mov edx, [params+P_CQ+20]         ; cq_off.cqes + cq_entries * 16
  mov ecx, [params+4]
  shl rcx, 4
  add rdx, rcx
  cmp rax, rdx
  cmovb rax, rdx
  mov rsi, rax
  xor r9d, r9d
  call .map
  mov r13, rax
  mov eax, [params+P_SQ]
  add rax, r13
  mov [rbx+R_SQHEAD], rax
  mov eax, [params+P_SQ+4]
  add rax, r13
  mov [rbx+R_SQTAIL], rax
  mov eax, [params+P_SQ+8]
  mov eax, [r13+rax]
  mov [rbx+R_SQMASK], eax
  mov eax, [params]
  mov [rbx+R_SQENT], eax
  mov eax, [params+P_SQ+16]
  add rax, r13
  mov [rbx+R_SQFLAGS], rax
  mov eax, [params+P_SQ+24]         ; SQ index array: slot i always holds SQE i
  add rax, r13
  xor ecx, ecx
.arr:
  mov [rax+rcx*4], ecx
  inc ecx
  cmp ecx, [params]
  jb .arr
  mov eax, [params+P_CQ]
  add rax, r13
  mov [rbx+R_CQHEAD], rax
  mov eax, [params+P_CQ+4]
  add rax, r13
  mov [rbx+R_CQTAIL], rax
  mov eax, [params+P_CQ+8]
  mov eax, [r13+rax]
  mov [rbx+R_CQMASK], eax
  mov eax, [params+P_CQ+20]
  add rax, r13
  mov [rbx+R_CQES], rax
  mov esi, [params]
  shl rsi, 6
  mov r9d, 0x10000000               ; IORING_OFF_SQES
  call .map
  mov [rbx+R_SQES], rax
  lea rdi, [rsrc]                   ; sparse registered file table, tsize slots
  xor eax, eax
  mov ecx, 4
  rep stosq
  mov eax, [tsize]
  mov [rsrc], eax
  mov dword [rsrc+4], 1             ; IORING_RSRC_REGISTER_SPARSE
  mov esi, REG_FILES2
  lea rdx, [rsrc]
  mov r10d, 32
  call .reg
  mov edi, NBUF*16                  ; provided buffer ring, group 0
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx+R_BR], rax
  lea rdi, [bufreg]
  mov rdx, rax
  xor eax, eax
  mov ecx, 5
  rep stosq
  mov [bufreg], rdx
  mov dword [bufreg+8], NBUF
  ; Some Ubuntu 6.8 kernels (0-136 and later, Launchpad #2162843) refuse a registration whose reserved words
  ; are all zero and accept resv[0] = 1; every other kernel takes the zeroes, so those are tried first.
  mov edi, [rbx+R_FD]
  mov esi, REG_PBUF_RING
  lea rdx, [bufreg]
  mov r10d, 1
  mov eax, SYS_io_uring_register
  syscall
  cmp eax, -22                      ; EINVAL
  jne .bufreg_rc
  mov qword [bufreg+16], 1
  mov esi, REG_PBUF_RING
  lea rdx, [bufreg]
  mov r10d, 1
  call .reg
  xor eax, eax
.bufreg_rc:
  test eax, eax
  js .fail
  lea rdi, [rsrc]                   ; register the ring's own fd: enters skip the fd lookup
  xor eax, eax
  mov ecx, 4
  rep stosq
  mov dword [rsrc], -1
  mov eax, [rbx+R_FD]
  mov [rsrc+8], rax
  mov edi, [rbx+R_FD]
  mov esi, REG_RING_FDS
  lea rdx, [rsrc]
  mov r10d, 1
  mov eax, SYS_io_uring_register
  syscall
  cmp eax, 1
  jne .noregfd
  mov eax, [rsrc]
  mov [rbx+R_EFD], eax
  mov dword [rbx+R_EFL], ENTER_REGISTERED_RING
.noregfd:
  mov edi, [tsize]
  shl rdi, 6
  call huge_alloc
  mov [rbx+R_CONNS], rax
  mov edi, NBUF*BSZ
  call huge_alloc
  mov [rbx+R_BUFS], rax
  xor r13d, r13d
.bufs:
  mov rdi, rbx
  mov esi, r13d
  call buf_put
  inc r13d
  cmp r13d, NBUF
  jb .bufs
  mov dword [rbx+R_N], 0
  mov dword [rbx+R_PEND], 0
  mov dword [rbx+R_READS], 0
  lea eax, [r12+1]
  mov [nrings], eax
  test r12d, r12d
  jz .ok
  mov eax, [tsize]                  ; "%u connections: added ring %d"
  sub eax, 16
  imul eax, r12d
  mov edx, eax
  mov ecx, r12d
  mov edi, 2
  lea rsi, [s_addring]
  xor eax, eax
  call dprintf
.ok:
  xor eax, eax
  jmp .ret
.map:                               ; rsi=len, r9=offset -> rax, fails the setup on error
  xor edi, edi
  mov edx, 3
  mov r10d, 0x8001                  ; MAP_SHARED | MAP_POPULATE
  mov r8d, [rbx+R_FD]
  mov eax, SYS_mmap
  syscall
  cmp rax, -4096
  ja .unwind
  ret
.reg:                               ; esi=opcode, rdx=arg, r10=nr_args, fails the setup on error
  mov edi, [rbx+R_FD]
  mov eax, SYS_io_uring_register
  syscall
  test eax, eax
  js .unwind
  ret
.unwind:
  add rsp, 8
.fail:
  ; shortcut: a failed ring leaks its mappings once, and no further rings are tried after it; retry if
  ; ring setup ever fails transiently
  mov edi, [rbx+R_FD]
  mov eax, SYS_close
  syscall
.fail0:
  mov [ring_cap], r12d
  mov eax, -1
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; conn_of: rdi=who -> rax = connection, or 0 once that connection has closed. clobbers rcx
conn_of:
  mov rax, rdi
  shr rax, 3
  and eax, 31
  shl eax, 7
  mov rax, [rings+rax+R_CONNS]
  mov rcx, rdi
  shr rcx, 8
  and ecx, 0xffffff
  shl rcx, 6
  add rax, rcx
  cmp byte [rax+C_OPEN], 0
  je .no
  mov rcx, rdi
  shr rcx, 32
  cmp [rax+C_GEN], ecx
  je .yes
.no:
  xor eax, eax
.yes:
  ret

; arm_recv: rdi=who. Multishot: one completion per chunk, into a kernel-picked provided buffer.
arm_recv:
  push rbx
  mov rbx, rdi
  shr rdi, 3
  and edi, 31
  call get_sqe
  mov byte [rax], OP_RECV
  mov byte [rax+1], SQE_FIXED_FILE | SQE_BUFFER_SELECT
  mov word [rax+2], RECV_MULTISHOT
  mov rcx, rbx
  shr rcx, 8
  and ecx, 0xffffff
  mov [rax+4], ecx
  mov [rax+32], rbx
  pop rbx
  ret

; send_op: rdi=send buffer. Sends what is left of it; its address is the completion tag.
send_op:
  push rbx
  mov rbx, rdi
  mov rdi, [rbx+S_WHO]
  shr rdi, 3
  and edi, 31
  call get_sqe
  mov byte [rax], OP_SEND
  mov byte [rax+1], SQE_FIXED_FILE
  mov rcx, [rbx+S_WHO]
  shr rcx, 8
  and ecx, 0xffffff
  mov [rax+4], ecx
  mov ecx, [rbx+S_OFF]
  lea rdx, [rbx+S_D+rcx]
  mov [rax+16], rdx
  mov edx, [rbx+S_LEN]
  sub edx, ecx
  mov [rax+24], edx
  mov dword [rax+28], MSG_NOSIGNAL
  mov [rax+32], rbx
  pop rbx
  ret

; conn_close: rdi=who. Ends the pending receive and send, then frees the slot (one linked chain). A send still
; in flight frees its own buffer when it completes; the generation bump makes late completions harmless.
conn_close:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi
  mov r14, rdi                      ; the receive's tag
  shr rdi, 3
  and edi, 31
  shl edi, 7
  lea r12, [rings+rdi]
  mov rax, rbx
  shr rax, 8
  and eax, 0xffffff
  shl rax, 6
  add rax, [r12+R_CONNS]
  mov r13, rax
  xor r15d, r15d                    ; the send in flight (its tag is its buffer), if any
  cmp byte [r13+C_SENDING], 0
  je .nosend
  mov r15, [r13+C_OUT]
.nosend:
  mov rdi, [r13+C_IN]
  call free
  mov rdi, [r13+C_Q]
  call free
  mov eax, [r13+C_GEN]
  inc eax
  xorps xmm0, xmm0
  movups [r13], xmm0
  movups [r13+16], xmm0
  movups [r13+32], xmm0
  mov [r13+C_GEN], eax
  dec dword [r12+R_N]
  mov rax, [r12+R_SQHEAD]           ; keep the chain in one submission
  mov ecx, [r12+R_TAIL]
  sub ecx, [rax]
  add ecx, 3
  cmp ecx, [r12+R_SQENT]
  jbe .room
  mov rdi, r12
  xor esi, esi
  xor edx, edx
  call ring_enter
.room:
  ; Cancel by tag: the kernel finds those in a hash table. Cancelling by descriptor would scan every
  ; pending operation on the ring, one per open connection.
  shr rbx, 3                        ; rbx = slot << 5 | k
  mov edi, ebx
  and edi, 31
  call get_sqe
  mov byte [rax], OP_ASYNC_CANCEL
  mov byte [rax+1], SQE_IO_HARDLINK
  mov dword [rax+4], -1
  mov [rax+16], r14                 ; addr: the tag to cancel (off must stay 0)
  mov qword [rax+32], T_NOP
  test r15, r15
  jz .close
  mov edi, ebx
  and edi, 31
  call get_sqe
  mov byte [rax], OP_ASYNC_CANCEL
  mov byte [rax+1], SQE_IO_HARDLINK
  mov dword [rax+4], -1
  mov [rax+16], r15
  mov qword [rax+32], T_NOP
.close:
  mov edi, ebx
  and edi, 31
  call get_sqe
  mov byte [rax], OP_CLOSE
  mov ecx, ebx
  shr ecx, 5
  and ecx, 0xffffff
  inc ecx                           ; file_index is slot + 1
  mov [rax+44], ecx
  mov qword [rax+32], T_NOP
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; queue_read: rdi=who, rsi=conn
queue_read:
  cmp byte [rsi+C_QUEUED], 0
  jne .r
  mov byte [rsi+C_QUEUED], 1
  mov rsi, rdi
  lea rdi, [v_readq]
  jmp vpush
.r:
  ret

; process: rdi=who, rsi=conn, rdx=buf, ecx=n. Serves every complete request in buf.
process:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 16               ; [rsp] used, [rsp+8] left
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov r14d, ecx
  mov r15, [awr]            ; start
  xor eax, eax
  mov [closing_flag], eax
  mov [arena_full], eax
  mov [hold_hit], eax
  lea rax, [r12+C_SENT100]
  mov [g_sent100], rax
  mov rsi, r13
  mov rdx, r14
  call serve
  mov [rsp], rax
  test rax, rax
  jns .ok
  mov [awr], r15
  jmp .close
.ok:
  mov rax, [awr]
  cmp rax, r15
  jbe .noseg
  mov ecx, [nsegs]
  shl rcx, 4
  mov [segs+rcx], rbx
  mov rdx, r15
  sub rdx, arena
  mov [segs+rcx+8], edx
  sub rax, r15
  mov [segs+rcx+12], eax
  inc dword [nsegs]
.noseg:
  mov al, [closing_flag]
  mov [r12+C_CLOSING], al
  xor ecx, ecx
  test al, al
  jnz .left
  mov rcx, r14
  sub rcx, [rsp]
.left:
  test rcx, rcx
  jz .noleft
  mov [rsp+8], rcx
  cmp r13, [r12+C_IN]
  je .move
  lea rdi, [rcx+BSZ]        ; keep the partial request until the rest arrives
  mov [r12+C_INCAP], edi
  call malloc
  test rax, rax
  jz .oom
  mov [r12+C_IN], rax
.move:
  mov rdi, [r12+C_IN]
  mov rsi, r13
  add rsi, [rsp]
  mov rcx, [rsp+8]
  rep movsb                 ; forward copy: safe when moving down inside the same buffer
  mov rcx, [rsp+8]
  mov [r12+C_INLEN], ecx
  cmp dword [arena_full], 0
  je .nodef
  lea rdi, [v_deferred]
  mov rsi, rbx
  call vpush
.nodef:
  cmp dword [hold_hit], 0
  je .chkclose
  lea rdi, [v_held]
  mov rsi, rbx
  call vpush
  jmp .chkclose
.noleft:
  mov rdi, [r12+C_IN]
  test rdi, rdi
  jz .chkclose
  call free
  mov qword [r12+C_IN], 0
  mov qword [r12+C_INLEN], 0        ; and C_INCAP
.chkclose:
  cmp [awr], r15
  jne .ret
  cmp byte [closing_flag], 0
  jne .close
  cmp byte [r12+C_EOF], 0
  je .ret
  cmp byte [r12+C_SENDING], 0
  jne .ret
  mov eax, [arena_full]
  or eax, [hold_hit]
  jnz .ret
.close:
  mov rdi, rbx
  call conn_close
.ret:
  add rsp, 16
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.oom:
  call abort

; on_data: rdi=who, rsi=conn, rdx=bytes, ecx=n. After a stall every connection has a request waiting. Letting
; them sit costs a socket buffer each (3 to 4 KB, toward TCP's limit) and answering them all makes one very
; long pass, so a pass answers ANSWERS and reads READS: the rest wait on their connection, in a copy the size
; of the request, on a first-in, first-out list.
on_data:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov r14d, ecx
  cmp byte [r12+C_CLOSING], 0       ; answering Connection: close, anything after it is ignored
  jne .ret
  xor r15d, r15d                    ; later
  cmp byte [r12+C_READY], 0
  jne .later
  cmp dword [answers_left], 0
  je .later
  dec dword [answers_left]
  cmp dword [r12+C_INLEN], 0
  jne .append
  cmp byte [r12+C_SENDING], 0
  jne .append
  call process
  jmp .ret
.later:
  mov r15d, 1
.append:
  mov eax, [r12+C_INCAP]
  sub eax, [r12+C_INLEN]
  cmp eax, r14d
  jae .fits
  mov esi, [r12+C_INLEN]
  add esi, r14d
  add esi, esi
  mov [r12+C_INCAP], esi
  mov rdi, [r12+C_IN]
  call realloc
  test rax, rax
  jz .oom
  mov [r12+C_IN], rax
.fits:
  mov rdi, [r12+C_IN]
  mov eax, [r12+C_INLEN]
  add rdi, rax
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  add [r12+C_INLEN], r14d
  cmp dword [r12+C_INLEN], 4*MAXREQ
  ja .close                 ; keeps sending but won't read its replies
  test r15d, r15d
  jz .now
  cmp byte [r12+C_READY], 0
  jne .ret
  mov byte [r12+C_READY], 1
  lea rdi, [v_readyq]
  mov rsi, rbx
  call vpush
  jmp .ret
.now:
  cmp byte [r12+C_SENDING], 0
  jne .ret
  mov rdi, rbx
  mov rsi, r12
  mov rdx, [r12+C_IN]
  mov ecx, [r12+C_INLEN]
  call process
  jmp .ret
.close:
  mov rdi, rbx
  call conn_close
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.oom:
  call abort

; run_ready: with the answers this pass has left, what earlier passes read, oldest first
run_ready:
  push rbx
  push r12
  sub rsp, 8
.l:
  cmp dword [answers_left], 0
  je .ret
  cmp dword [v_readyq+V_N], 0
  je .ret
  lea rdi, [v_readyq]
  call fifo_pop
  mov rbx, rax
  mov rdi, rax
  call conn_of
  test rax, rax
  jz .l
  mov byte [rax+C_READY], 0
  cmp dword [rax+C_INLEN], 0
  je .l
  cmp byte [rax+C_SENDING], 0
  jne .l
  dec dword [answers_left]
  mov rdi, rbx
  mov rsi, rax
  mov rdx, [rax+C_IN]
  mov ecx, [rax+C_INLEN]
  call process
  jmp .l
.ret:
  add rsp, 8
  pop r12
  pop rbx
  ret

; Send buffers: one send in flight per connection keeps responses in order; later ones queue behind it.
; Sends finish inline, so a small LIFO pool keeps handing out the buffer most recently written, still in cache.
; sop_new: edi=n -> rax buffer with S_CAP set
sop_new:
  sub rsp, 8
  cmp edi, POOL_CAP
  ja .big
  mov eax, [npool]
  test eax, eax
  jz .alloc
  dec eax
  mov [npool], eax
  mov rax, [pool+rax*8]
  add rsp, 8
  ret
.alloc:
  mov edi, POOL_CAP
.big:
  push rdi
  sub rsp, 8
  add edi, S_D
  call malloc
  add rsp, 8
  pop rdi
  test rax, rax
  jz .oom
  mov [rax+S_CAP], edi
  add rsp, 8
  ret
.oom:
  call abort

; sop_free: rdi=buffer
sop_free:
  cmp dword [rdi+S_CAP], POOL_CAP
  jne free
  mov eax, [npool]
  cmp eax, POOL_MAX
  jae free
  mov [pool+rax*8], rdi
  inc eax
  mov [npool], eax
  ret

; send_bytes: rdi=who, rsi=conn, rdx=bytes, ecx=n. Each send owns a copy, so the arena is reused at once.
send_bytes:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 16               ; [rsp] queued length, [rsp+8] new capacity
  mov rbx, rdi
  mov r12, rsi
  mov r13, rdx
  mov r14d, ecx
  cmp byte [r12+C_SENDING], 0
  jne .queue
  mov edi, r14d
  call sop_new
  mov r15, rax
  mov [rax+S_WHO], rbx
  mov [rax+S_LEN], r14d
  mov dword [rax+S_OFF], 0
  lea rdi, [rax+S_D]
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  mov byte [r12+C_SENDING], 1
  mov [r12+C_OUT], r15
  mov rdi, r15
  call send_op
  jmp .ret
.queue:
  mov r15, [r12+C_Q]
  xor eax, eax
  test r15, r15
  jz .grow
  mov eax, [r15+S_CAP]
  sub eax, [r15+S_LEN]
  cmp eax, r14d
  jae .append
  mov eax, [r15+S_LEN]
.grow:
  mov [rsp], eax
  add eax, r14d
  add eax, eax
  mov [rsp+8], eax
  mov rdi, r15
  lea rsi, [rax+S_D]
  call realloc
  test rax, rax
  jz .oom
  mov r15, rax
  mov [r12+C_Q], rax
  mov [rax+S_WHO], rbx
  mov ecx, [rsp]
  mov [rax+S_LEN], ecx
  mov dword [rax+S_OFF], 0
  mov ecx, [rsp+8]
  mov [rax+S_CAP], ecx
.append:
  mov eax, [r15+S_LEN]
  lea rdi, [r15+S_D+rax]
  mov rsi, r13
  mov ecx, r14d
  rep movsb
  add [r15+S_LEN], r14d
.ret:
  add rsp, 16
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret
.oom:
  call abort

; send_done: rdi=send buffer, esi=res
send_done:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi
  mov r13d, esi
  mov r14, [rbx+S_WHO]
  mov rdi, r14
  call conn_of
  mov r12, rax
  test r12, r12
  jz .limbo
  test r13d, r13d
  jle .done
  add [rbx+S_OFF], r13d
  mov eax, [rbx+S_OFF]
  cmp eax, [rbx+S_LEN]
  jae .done
  mov rdi, rbx              ; short send: the rest
  call send_op
  jmp .ret
.limbo:                     ; a closed connection's cancel may still be queued, naming this address: it stays
  lea rdi, [v_limbo]        ; unused until that has gone in
  mov rsi, rbx
  call vpush
  jmp .ret
.done:
  mov rdi, rbx
  call sop_free
  test r13d, r13d
  jg .sent
  mov byte [r12+C_SENDING], 0
  jmp .close
.sent:
  mov rdi, [r12+C_Q]
  test rdi, rdi
  jz .idle
  mov qword [r12+C_Q], 0
  mov [r12+C_OUT], rdi
  call send_op
  jmp .ret
.idle:
  mov byte [r12+C_SENDING], 0
  cmp byte [r12+C_CLOSING], 0
  jne .close
  mov ecx, [r12+C_INLEN]
  test ecx, ecx
  jz .eof
  mov rdi, r14              ; input that arrived while sending
  mov rsi, r12
  mov rdx, [r12+C_IN]
  call process
  jmp .ret
.eof:
  cmp byte [r12+C_EOF], 0
  je .ret
.close:
  mov rdi, r14
  call conn_close
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; flush_batch: commits the batch, then sends every response produced in it
flush_batch:
  push rbx
  push r12
  push r13
  cmp dword [in_txn], 0
  je .noc
  call td_end
  mov rdi, [st_commit]
  call step_reset
  test eax, eax
  jz .committed
  mov rdi, [st_rollback]
  call step_reset
  cmp dword [txn_writes], 0
  je .committed
  mov dword [batch_failed], 1
.committed:
  mov dword [in_txn], 0
  mov dword [txn_writes], 0
.noc:
  xor r12d, r12d
.segloop:
  cmp r12d, [nsegs]
  jae .segdone
  mov r13d, r12d
  shl r13, 4
  mov rbx, [segs+r13]
  mov rdi, rbx
  call conn_of
  test rax, rax
  jz .segnext
  cmp dword [batch_failed], 0
  je .send
  mov rdi, rbx              ; the batch wasn't committed: never claim it was
  call conn_close
  jmp .segnext
.send:
  mov rdi, rbx
  mov rsi, rax
  mov edx, [segs+r13+8]
  add rdx, arena
  mov ecx, [segs+r13+12]
  call send_bytes
.segnext:
  inc r12d
  jmp .segloop
.segdone:
  mov dword [nsegs], 0
  mov qword [awr], arena
  mov dword [batch_failed], 0
  pop r13
  pop r12
  pop rbx
  ret

; arm_accepts: single-shot accepts, a few in flight, on the first ring with room. An accept that finds its table
; full has already taken the connection and can only drop it, so tables are never filled to the last 16 slots
; (closes still in flight may hold a few), and a multishot accept can't be stopped in time. Each listener keeps
; its own share armed: connections are hashed to listeners, so a listener with none armed would leave its queue
; waiting while accepts armed on idle listeners never complete.
arm_accepts:
  push rbx
  push r12
  push r13
  push r14
  push r15
  xor ebx, ebx              ; k
  xor r13d, r13d            ; l
.lis:
  cmp r13d, NLISTEN
  jae .done
.l:
  cmp dword [armed+r13*4], ACCEPTS
  jae .nextl
  cmp ebx, [ring_cap]
  jae .nextl
  cmp ebx, [nrings]
  jb .have
  mov edi, ebx
  call ring_init
  test eax, eax
  jnz .done                 ; retried after the next event
.have:
  mov r12d, ebx
  shl r12d, 7
  lea r12, [rings+r12]
  mov eax, [r12+R_N]
  add eax, [r12+R_PEND]
  add eax, 16
  cmp eax, [tsize]
  jb .arm
  inc ebx
  jmp .l
.arm:
  mov edi, ebx
  call get_sqe
  mov byte [rax], OP_ACCEPT
  mov ecx, [lfds+r13*4]
  mov [rax+4], ecx
  mov dword [rax+44], FILE_INDEX_ALLOC
  lea rcx, [r13*8+T_ACCEPT]
  mov [rax+32], rcx
  inc dword [r12+R_PEND]
  inc dword [armed+r13*4]
  jmp .l
.nextl:
  inc r13d
  jmp .lis
.done:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; arm_wal_msg: the checkpoint thread's reply arrives as a completion on ring 0
arm_wal_msg:
  sub rsp, 8
  xor edi, edi
  call get_sqe
  mov byte [rax], OP_READ
  mov ecx, [td_wal_replies]
  mov [rax+4], ecx
  lea rcx, [wal_msg]
  mov [rax+16], rcx
  mov dword [rax+24], 8
  mov qword [rax+32], T_WAL
  add rsp, 8
  ret

; drain: edi=k -> eax = completions handled in ring k (tail read once, so at most CQ_ENTRIES per call).
; user_data: low 3 bits tag the operation; a send's tag is its buffer address (tag 0); a receive's is
; T_RECV | ring << 3 | slot << 8 | generation << 32, which is also the connection's "who".
drain:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 16               ; [rsp] res, [rsp+8] flags, [rsp+12] count
  mov r12d, edi
  shl edi, 7
  lea rbx, [rings+rdi]
  mov rax, [rbx+R_CQHEAD]
  mov r13d, [rax]
  mov rax, [rbx+R_CQTAIL]
  mov r14d, [rax]
  mov [rsp+12], r14d
  sub [rsp+12], r13d
.l:
  cmp r13d, r14d
  je .done
  mov eax, r13d
  and eax, [rbx+R_CQMASK]
  shl rax, 4
  add rax, [rbx+R_CQES]
  mov rdi, [rax]
  movsxd rsi, dword [rax+8]
  mov edx, [rax+12]
  inc r13d
  mov rax, [rbx+R_CQHEAD]   ; the fields are copied, so the slot can go back now
  mov [rax], r13d
  mov eax, edi
  and eax, 7
  cmp eax, T_RECV
  je .recv
  test eax, eax
  jz .send
  cmp eax, T_ACCEPT
  je .accept
  cmp eax, T_READ
  je .read
  cmp eax, T_WAL
  jne .l
  mov eax, 1                ; the pass's outcome, or "frames left" if the read failed
  cmp esi, 8
  jne .walset
  mov rax, [wal_msg]
.walset:
  mov [wal_outcome], rax
  call arm_wal_msg
  jmp .l
.send:
  call send_done
  jmp .l
.accept:
  dec dword [rbx+R_PEND]
  shr rdi, 3
  dec dword [armed+rdi*4]
  test esi, esi
  js .l
  mov rax, [rbx+R_CONNS]
  mov ecx, esi
  shl rcx, 6
  add rax, rcx
  mov byte [rax+C_OPEN], 1
  inc dword [rbx+R_N]
  mov edi, [rax+C_GEN]
  shl rdi, 32
  shl esi, 8
  or rdi, rsi
  mov eax, r12d
  shl eax, 3
  or eax, T_RECV
  or rdi, rax
  call arm_recv
  jmp .l
.recv:
  mov r15, rdi
  mov [rsp], rsi
  mov [rsp+8], edx
  call .data
  test byte [rsp+8], CQE_F_MORE
  jnz .l
  mov rdi, r15
  call conn_of
  test rax, rax
  jz .l
  mov rcx, [rsp]
  cmp rcx, 0
  jg .queue                 ; out of buffers: rearming every receive at once would repeat that, so the rest
  cmp rcx, -105             ; is read in bounded rounds (ENOBUFS)
  je .queue
  jmp .eof
.read:                      ; a single-shot MSG_DONTWAIT read from read_rounds
  and rdi, -8
  or rdi, T_RECV
  mov r15, rdi
  mov [rsp], rsi
  mov [rsp+8], edx
  dec dword [rbx+R_READS]
  call .data
  mov rdi, r15
  call conn_of
  test rax, rax
  jz .l
  mov rcx, [rsp]
  cmp rcx, BSZ              ; a full buffer: there may be more
  je .queue
  cmp rcx, -105
  je .queue
  cmp rcx, 0
  jg .rearm
  cmp rcx, -11              ; EAGAIN: drained, back to the multishot receive
  jne .eof
.rearm:
  mov rdi, r15
  call arm_recv
  jmp .l
.queue:
  mov rdi, r15
  mov rsi, rax
  call queue_read
  jmp .l
.eof:                       ; the same pass may hold this client's last reply
  mov byte [rax+C_EOF], 1
  lea rdi, [v_eofs]
  mov rsi, r15
  call vpush
  jmp .l
.data:                      ; r15=who, [rsp+8]=res, [rsp+16]=flags (one call deeper): hand the bytes over,
  test byte [rsp+16], CQE_F_BUFFER  ; then the buffer back to the ring
  jz .nodata
  sub rsp, 8
  mov rdi, r15
  call conn_of
  test rax, rax
  jz .putbuf
  cmp qword [rsp+16], 0
  jle .putbuf
  mov rdi, r15
  mov rsi, rax
  mov edx, [rsp+24]
  shr edx, 16
  shl rdx, BSZ_SHIFT
  add rdx, [rbx+R_BUFS]
  mov ecx, [rsp+16]
  call on_data
.putbuf:
  mov rdi, rbx
  mov esi, [rsp+24]
  shr esi, 16
  call buf_put
  add rsp, 8
.nodata:
  ret
.done:
  mov eax, [rsp+12]
  add rsp, 16
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; read_rounds: one MSG_DONTWAIT read per queued connection, never more on a ring than it has buffers. They
; finish inside the submit. -> eax completions seen
read_rounds:
  push rbx
  push r12
  push r13
  push r14
  push r15
  xor r14d, r14d            ; seen
  mov r15d, READS           ; budget
.round:
  test r15d, r15d
  jle .ret
  cmp dword [v_readq+V_N], 0
  je .ret
  xor r12d, r12d            ; issued
  mov r13d, [v_readq+V_N]
  sub r13d, [v_readq+V_HEAD] ; stop
.one:
  test r13d, r13d
  jz .submit
  cmp r12d, r15d
  jae .submit
  dec r13d
  lea rdi, [v_readq]
  call fifo_pop
  mov rbx, rax
  mov rdi, rax
  call conn_of
  test rax, rax
  jz .one
  mov rcx, rbx
  shr rcx, 3
  and ecx, 31
  shl ecx, 7
  cmp dword [rings+rcx+R_READS], NBUF
  jb .issue
  lea rdi, [v_readq]
  mov rsi, rbx
  call vpush
  jmp .one
.issue:
  mov byte [rax+C_QUEUED], 0
  inc dword [rings+rcx+R_READS]
  inc r12d
  mov rdi, rbx
  shr rdi, 3
  and edi, 31
  call get_sqe
  mov byte [rax], OP_RECV
  mov byte [rax+1], SQE_FIXED_FILE | SQE_BUFFER_SELECT
  mov rcx, rbx
  shr rcx, 8
  and ecx, 0xffffff
  mov [rax+4], ecx
  mov dword [rax+28], MSG_DONTWAIT
  mov rcx, rbx
  and rcx, -8
  or rcx, T_READ
  mov [rax+32], rcx
  jmp .one
.submit:
  test r12d, r12d
  jz .ret
  sub r15d, r12d
  xor ebx, ebx
.rings:
  cmp ebx, [nrings]
  jae .round
  mov eax, ebx
  shl eax, 7
  lea rdi, [rings+rax]
  xor esi, esi
  mov edx, ENTER_GETEVENTS
  call ring_enter
  mov edi, ebx
  call drain
  add r14d, eax
  inc ebx
  jmp .rings
.ret:
  mov eax, r14d
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ---------- checkpoints: the loop decides, the shim's worker thread copies ----------
; A log can only restart once all of it is copied, which steady writes prevent, so if frames arrived during a
; pass the loop stops taking POSTs (reads go on) for one more pass. Past WAL_MAX frames it waits for that pass
; however long it takes, because a saturated CPU starves the worker and the log would otherwise grow without bound.
wal_ask:
  sub rsp, 8
  mov dword [wal_busy], 1
  mov eax, [td_wal_frames]
  mov [wal_asked_at], eax
  mov qword [one64], 1
  mov edi, [td_wal_asks]
  lea rsi, [one64]
  mov edx, 8
  mov eax, SYS_write
  syscall
  cmp rax, 8
  je .ok
  mov dword [wal_busy], 0
.ok:
  add rsp, 8
  ret

; wal_copied: rdi=outcome (1 frames left, 2 all copied)
wal_copied:
  sub rsp, 8
  mov dword [wal_busy], 0
  cmp rdi, 2                ; all copied, and nothing committed since the pass was asked for: the next
  jne .notall               ; write restarts the log
  mov eax, [td_wal_frames]
  cmp eax, [wal_asked_at]
  jne .notall
  mov dword [td_wal_frames], 0
  jmp .release
.notall:
  cmp dword [wal_last_pass], 0  ; POSTs were let go early, or the pass failed: try again later
  jne .release
  mov dword [wal_last_pass], 1
  mov dword [hold_posts], 1
  mov qword [post_deadline], 0
  cmp dword [td_wal_frames], WAL_MAX
  jge .ask
  call mono_ns
  add rax, POST_WAIT_NS
  mov [post_deadline], rax
.ask:
  call wal_ask
  jmp .ret
.release:
  mov dword [wal_last_pass], 0
  mov dword [hold_posts], 0
.ret:
  add rsp, 8
  ret

; wal_kick: once WALKICK_MB of new log is written, queue its writeback (SYNC_FILE_RANGE_WRITE on an io-wq thread)
; so dirty WAL pages never pile up to the kernel's dirty threshold, where a write on the loop would wait. Only
; starts writeback, never waits: durability is still SQLite's own syncs.
wal_kick:
  sub rsp, 8
  mov ecx, [td_walkick_mb]
  test ecx, ecx
  jz .ret
  cmp dword [td_kick_fd], 0
  jl .ret
  movsxd rax, dword [td_wal_frames]
  imul rax, rax, 4096+24
  add rax, 32
  and rax, -4096            ; whole pages only, the last is still filling
  cmp rax, [kicked_to]
  jge .grown
  mov qword [kicked_to], 0  ; the log restarted
.grown:
  mov rdx, rax
  sub rdx, [kicked_to]
  shl rcx, 20
  cmp rdx, rcx
  jl .ret
  push rax
  push rdx
  xor edi, edi
  call get_sqe
  pop rdx
  pop rcx
  mov byte [rax], OP_SYNC_FILE_RANGE
  mov byte [rax+1], SQE_ASYNC
  mov esi, [td_kick_fd]
  mov [rax+4], esi
  mov rsi, [kicked_to]
  mov [rax+8], rsi
  mov [rax+24], edx
  mov dword [rax+28], 2     ; SYNC_FILE_RANGE_WRITE
  mov qword [rax+32], T_NOP
  mov [kicked_to], rcx
.ret:
  add rsp, 8
  ret

; wal_after_commit: between batches, with no transaction open: a write only restarts the log from a read
; transaction begun after the last copy, so POSTs can't be let go in the middle of a batch.
wal_after_commit:
  sub rsp, 8
  call wal_kick
  mov rdi, [wal_outcome]
  test rdi, rdi
  jz .noout
  mov qword [wal_outcome], 0
  call wal_copied
.noout:
  cmp dword [td_wal_frames], WAL_MAX
  jl .notmax
  cmp dword [wal_busy], 0
  je .notmax
  mov dword [hold_posts], 1
  mov qword [post_deadline], 0
  jmp .ret
.notmax:
  cmp dword [wal_busy], 0
  jne .ret
  cmp dword [td_wal_frames], WAL_START
  jl .ret
  call wal_ask
.ret:
  add rsp, 8
  ret

; wal_hold_over -> eax 1 when POSTs may go again
wal_hold_over:
  sub rsp, 8
  cmp dword [hold_posts], 0
  je .over
  cmp qword [post_deadline], 0
  je .held
  call mono_ns
  cmp rax, [post_deadline]
  jle .held
  mov dword [hold_posts], 0
.over:
  mov eax, 1
  add rsp, 8
  ret
.held:
  xor eax, eax
  add rsp, 8
  ret

; run_list: rdi=list. Takes it (later pushes start a new one), processes each connection that still has input
; and no send in flight, frees it, then commits and sends.
run_list:
  push rbx
  push r12
  push r13
  call vtake
  mov r12, [v_tmp+V_A]
  mov r13d, [v_tmp+V_N]
  xor ebx, ebx
.l:
  cmp ebx, r13d
  jae .done
  mov rdi, [r12+rbx*8]
  call conn_of
  test rax, rax
  jz .next
  cmp dword [rax+C_INLEN], 0
  je .next
  cmp byte [rax+C_SENDING], 0
  jne .next
  mov rdi, [r12+rbx*8]
  mov rsi, rax
  mov rdx, [rax+C_IN]
  mov ecx, [rax+C_INLEN]
  call process
.next:
  inc ebx
  jmp .l
.done:
  mov rdi, r12
  call free
  call flush_batch
  pop r13
  pop r12
  pop rbx
  ret

; run loop: reached by jmp from main (rsp 16-byte aligned), never returns. r15d = completions seen by the last pass.
run:
  xor r15d, r15d
.loop:
  call arm_accepts
  mov ebx, [rings+R_N]      ; open connections
  mov r12d, 1
.extra:                     ; every other ring: one non-blocking enter that also runs its completion work
  cmp r12d, [nrings]
  jae .wait
  mov eax, r12d
  shl eax, 7
  lea rdi, [rings+rax]
  add ebx, [rdi+R_N]
  xor esi, esi
  mov edx, ENTER_GETEVENTS
  call ring_enter
  inc r12d
  jmp .extra
.wait:
  lea rdi, [rings]
  mov eax, [v_readq+V_N]
  or eax, [v_readyq+V_N]
  jz .maywait
  xor esi, esi              ; work is waiting here: no sleeping
  mov edx, ENTER_GETEVENTS
  call ring_enter
  jmp .entered
.maywait:
  ; Busy, or more than one ring to look at, or POSTs held: wait for ring 0's share of a batch or a short timeout,
  ; whichever is first. The target is capped by the open connections so a few clients without think time never
  ; sit out the timer.
  cmp r15d, 4
  jae .timed
  cmp dword [nrings], 1
  ja .timed
  cmp dword [v_held+V_N], 0
  jne .timed
  mov esi, 1
  mov edx, ENTER_GETEVENTS
  call ring_enter
  jmp .entered
.timed:
  mov esi, 1
  cmp r15d, 4
  jb .cap
  mov eax, WAIT_N
  xor edx, edx
  div dword [nrings]
  lea esi, [rax+1]
.cap:
  mov eax, ebx
  xor edx, edx
  mov ecx, 3
  div ecx
  inc eax
  cmp esi, eax
  cmova esi, eax
  lea rdi, [rings]
  mov edx, ENTER_GETEVENTS | ENTER_EXT_ARG
  lea r8, [wait_arg]
  mov r9d, 24
  call ring_enter.arg
.entered:
  mov rbx, rax
  mov r12d, [v_limbo+V_N]   ; those cancels are in now
  mov r13, [v_limbo+V_A]
.limbo:
  test r12d, r12d
  jz .limbo_done
  dec r12d
  mov rdi, [r13+r12*8]
  call sop_free
  jmp .limbo
.limbo_done:
  mov dword [v_limbo+V_N], 0
  test rbx, rbx
  jns .woke
  cmp rbx, -4               ; EINTR
  je .woke
  cmp rbx, -16              ; EBUSY
  je .woke
  cmp rbx, -62              ; ETIME: the timed wait ran out
  je .woke
  cmp rbx, -11              ; EAGAIN
  je .woke
  mov edi, 2
  lea rsi, [s_enterfail]
  mov edx, ebx
  neg edx
  xor eax, eax
  call dprintf
  mov edi, 1
  call exit
.woke:
  xor edi, edi
  call time
  mov [now_s], rax
  xor r15d, r15d
  mov dword [answers_left], ANSWERS
  ; Requests that just arrived go first. After a stall the backlog has missed any latency target already;
  ; answered first, it would make everything behind it miss too.
  xor r12d, r12d
.drain:
  cmp r12d, [nrings]
  jae .drained
  mov edi, r12d
  call drain
  add r15d, eax
  inc r12d
  jmp .drain
.drained:
  call run_ready
  call read_rounds
  add r15d, eax
  call flush_batch
  cmp dword [v_eofs+V_N], 0 ; clients that finished sending: close once what they sent has been answered
  je .defer
  lea rdi, [v_eofs]
  call vtake
  mov r13, [v_tmp+V_A]
  mov r14d, [v_tmp+V_N]
  xor r12d, r12d
.eof:
  cmp r12d, r14d
  jae .eof_done
  mov rdi, [r13+r12*8]
  call conn_of
  test rax, rax
  jz .eof_next
  cmp byte [rax+C_SENDING], 0   ; the send's completion takes it from here
  jne .eof_next
  mov rdi, [r13+r12*8]
  cmp dword [rax+C_INLEN], 0
  je .eof_close
  mov rsi, rax
  mov rdx, [rax+C_IN]
  mov ecx, [rax+C_INLEN]
  call process
  jmp .eof_next
.eof_close:
  call conn_close
.eof_next:
  inc r12d
  jmp .eof
.eof_done:
  mov dword [v_tmp+V_N], 0      ; keep the array for the next pass
  mov rax, [v_tmp+V_A]
  mov [v_eofs+V_A], rax
  mov eax, [v_tmp+V_CAP]
  mov [v_eofs+V_CAP], eax
  call flush_batch
.defer:                     ; requests held back by a full arena
  cmp dword [v_deferred+V_N], 0
  je .wal
  lea rdi, [v_deferred]
  call run_list
  jmp .defer
.wal:
  call wal_after_commit
  cmp dword [v_held+V_N], 0
  je .loop
  call wal_hold_over
  test eax, eax
  jz .loop
  lea rdi, [v_held]
  call run_list
  jmp .loop

; main(edi=argc, rsi=argv, rdx=envp)
main:
  push rbx
  push r12
  push r13
  mov [envp_ptr], rdx
  lea rax, [no_conn]
  mov [g_sent100], rax
  lea rdi, [env_sqlite]
  mov rsi, env_sqlite_len
  call getenv_
  test rax, rax
  jz .missing
  mov rbx, rax
  lea rdi, [env_secret]
  mov rsi, env_secret_len
  call getenv_
  test rax, rax
  jz .missing
  mov [secret_ptr], rax
  lea rdi, [env_host]
  mov rsi, env_host_len
  call getenv_
  test rax, rax
  jnz .haveh
  lea rax, [default_host]
.haveh:
  mov [g_host], rax
  lea rdi, [env_port]
  mov rsi, env_port_len
  call getenv_
  test rax, rax
  jz .defport
  mov rdi, rax
  call atoi_
  mov dword [g_port], eax
  jmp .portdone
.defport:
  mov dword [g_port], 3000
.portdone:
  ; RLIMIT_NOFILE soft -> hard; the kernel caps one registered file table at it
  xor edi, edi
  mov esi, 7
  xor edx, edx
  lea r10, [rlbuf]
  mov eax, SYS_prlimit64
  syscall
  mov rax, [rlbuf+8]
  mov [rlbuf], rax
  xor edi, edi
  mov esi, 7
  lea rdx, [rlbuf]
  xor r10, r10
  mov eax, SYS_prlimit64
  syscall
  xor edi, edi
  mov esi, 7
  xor edx, edx
  lea r10, [rlbuf]
  mov eax, SYS_prlimit64
  syscall
  mov rax, [rlbuf]
  mov ecx, 1 << 20
  cmp rax, rcx
  cmova rax, rcx
  mov [tsize], eax
  mov dword [ring_cap], MAXRINGS
  lea rdi, [b64tab]         ; base64url table
  mov rcx, 256
  mov al, -1
  rep stosb
  xor rcx, rcx
.b64:
  lea rsi, [b64alpha]
  movzx eax, byte [rsi+rcx]
  lea rdx, [b64tab]
  mov [rdx+rax], cl
  inc rcx
  cmp rcx, 64
  jb .b64
  call init_wants
  mov rdi, [secret_ptr]
  call hmac_key
  xor edi, edi
  call time
  mov [started], rax
  mov [now_s], rax
  mov qword [awr], arena
  lea rdi, [arena+0x1fffff] ; the arena on huge pages where it covers a whole one
  and rdi, -0x200000
  mov esi, 0x200000
  mov edx, 14
  mov eax, SYS_madvise
  syscall
  mov rdi, rbx
  call db_open
  call listen_start
  xor edi, edi
  call ring_init
  test eax, eax
  jnz .nouring
  call arm_wal_msg
  jmp run
.nouring:
  mov edi, 2
  lea rsi, [s_nouring]
  mov edx, s_nouring_len
  mov eax, SYS_write
  syscall
.missing:
  mov edi, 1
  call exit

; listen_start: NLISTEN SO_REUSEPORT listeners, each with its own accept queue of somaxconn; TCP_NODELAY is
; inherited by accepted sockets
listen_start:
  push rbx
  mov word [sockaddr], 2    ; AF_INET
  mov eax, dword [g_port]
  xchg al, ah               ; htons
  mov word [sockaddr+2], ax
  mov rdi, [g_host]
  call parse_ipv4
  mov qword [sockaddr+8], 0
  mov dword [one_val], 1
  xor ebx, ebx
.l:
  mov edi, 2
  mov esi, 1 | 0x80000      ; SOCK_STREAM | SOCK_CLOEXEC
  xor edx, edx
  mov eax, SYS_socket
  syscall
  test eax, eax
  js .fatal
  mov [lfds+rbx*4], eax
  mov esi, 1                ; SOL_SOCKET
  mov edx, 2                ; SO_REUSEADDR
  call .opt
  mov esi, 1
  mov edx, 15               ; SO_REUSEPORT
  call .opt
  mov esi, 6                ; IPPROTO_TCP
  mov edx, 1                ; TCP_NODELAY
  call .opt
  mov edi, [lfds+rbx*4]
  lea rsi, [sockaddr]
  mov edx, 16
  mov eax, SYS_bind
  syscall
  test eax, eax
  js .fatal
  mov edi, [lfds+rbx*4]
  mov esi, 65535
  mov eax, SYS_listen
  syscall
  test eax, eax
  js .fatal
  inc ebx
  cmp ebx, NLISTEN
  jb .l
  pop rbx
  ret
.opt:
  mov edi, [lfds+rbx*4]
  lea r10, [one_val]
  mov r8d, 4
  mov eax, SYS_setsockopt
  syscall
  ret
.fatal:
  mov edi, 2
  lea rsi, [s_bindfail]
  mov edx, s_bindfail_len
  mov eax, SYS_write
  syscall
  mov edi, 1
  call exit

; atoi_: rdi=ptr -> eax
atoi_:
  xor eax, eax
.l:
  movzx edx, byte [rdi]
  cmp dl, '0'
  jb .d
  cmp dl, '9'
  ja .d
  imul eax, eax, 10
  sub dl, '0'
  movzx edx, dl
  add eax, edx
  inc rdi
  jmp .l
.d:
  ret

section .note.GNU-stack noalloc noexec nowrite progbits
