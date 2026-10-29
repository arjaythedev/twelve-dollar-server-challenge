; Feed API server in x86-64 assembly (NASM, System V ABI, Linux).
; All application logic is assembly: HTTP/1.1 parsing, routing, strict JSON validation + string
; decode, base64url, SHA-256/HMAC, JWT checks, JSON output, integer formatting, and an io_uring loop
; driven by raw io_uring_setup/enter/register syscalls on mmap'd rings. The only external code is
; SQLite (pinned amalgamation, compiled in by build.sh); its allocator also serves our buffers.
; Design mirrors submissions/c-iouring-unorig-username/server.c: one process, one thread, accepted
; sockets as registered descriptors (more rings as tables fill), one deferred transaction per
; completion batch, responses held until COMMIT, WAL checkpoints in a helper process (--ckpt).

bits 64
default rel

%define MAXREQ       65536          ; per-request header+body cap
%define BATCH_OUT    (4*1024*1024)  ; response bytes held per batch until its commit
%define MAXSEG       65536          ; responses per batch
%define MAXRINGS     32
%define SQ_ENTRIES   4096
%define CQ_ENTRIES   16384
%define MAXCQE       (MAXRINGS*CQ_ENTRIES)  ; completions per loop iteration, bounds rearm/deferred
%define NBUF         4096           ; provided receive buffers per ring
%define BSZ          2048
%define ACCEPTS      16

; connection (64 bytes), per ring slot
%define C_IN      0      ; q: partial request, or input held while a send is in flight
%define C_Q       8      ; q: responses queued behind the send in flight
%define C_INLEN   16     ; d
%define C_INCAP   20     ; d
%define C_GEN     24     ; d
%define C_OPEN    28     ; b
%define C_CLOSING 29     ; b
%define C_SENDING 30     ; b
%define C_OUT     32     ; q: the send in flight
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

; io_uring ABI (include/uapi/linux/io_uring.h)
%define SYS_io_uring_setup 425
%define SYS_io_uring_enter 426
%define SYS_io_uring_register 427
%define SYS_eventfd2 290
%define P_SQ 40                     ; io_uring_params.sq_off
%define P_CQ 80                     ; io_uring_params.cq_off
%define SETUP_FLAGS (8 | 128 | 4096)  ; CQSIZE | SUBMIT_ALL | SINGLE_ISSUER
%define SETUP_DEFER_TASKRUN 8192
%define REG_EVENTFD 4
%define REG_FILES2 13
%define REG_PBUF_RING 22
%define OP_ACCEPT 13
%define OP_ASYNC_CANCEL 14
%define OP_CLOSE 19
%define OP_READ 22
%define OP_SEND 26
%define OP_RECV 27
%define SQE_FIXED_FILE 1
%define SQE_IO_HARDLINK 8
%define SQE_BUFFER_SELECT 32
%define RECV_MULTISHOT 2
%define FILE_INDEX_ALLOC 0xffffffff
%define CQE_F_BUFFER 1
%define CQE_F_MORE 2
%define SQ_CQ_OVERFLOW 2
%define ENTER_GETEVENTS 1
%define ENTER_EXT_ARG 8
%define MSG_NOSIGNAL 0x4000
; completion tags (low 3 bits of user_data; 0 is a send, tagged by its buffer address)
%define T_ACCEPT 1
%define T_RECV 2
%define T_NOP 3
%define T_EVFD 4

; sqlite constants
%define SQLITE_ROW    100
%define SQLITE_DONE   101
%define SQLITE_OPEN_RW 2
%define SQLITE_PREP_PERSISTENT 1

; syscalls
%define SYS_write 1
%define SYS_close 3
%define SYS_mmap 9
%define SYS_rt_sigaction 13
%define SYS_socket 41
%define SYS_bind 49
%define SYS_listen 50
%define SYS_setsockopt 54
%define SYS_nanosleep 35
%define SYS_exit_group 231
%define SYS_time 201
%define SYS_prlimit64 302

extern sqlite3_initialize, sqlite3_open_v2, sqlite3_busy_timeout, sqlite3_exec
extern sqlite3_prepare_v3, sqlite3_step, sqlite3_reset, sqlite3_bind_int64, sqlite3_bind_text
extern sqlite3_column_int64, sqlite3_column_text, sqlite3_column_bytes, sqlite3_changes
extern sqlite3_get_autocommit, sqlite3_errmsg, sqlite3_wal_checkpoint_v2
extern sqlite3_malloc64, sqlite3_realloc64, sqlite3_free

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
DEFSTR sql_fids, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20",0
DEFSTR sql_flikes, "SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?",0
DEFSTR sql_frows, "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ? AND ?",0
DEFSTR sql_ins, "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",0
DEFSTR sql_like, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING",0
DEFSTR sql_exists, "SELECT 1 FROM posts WHERE id = ?",0
DEFSTR sql_one, "SELECT 1",0
DEFSTR sql_begin, "BEGIN",0
DEFSTR sql_commit, "COMMIT",0
DEFSTR sql_rollback, "ROLLBACK",0
; wal_autocheckpoint is only a backstop: the --ckpt helper keeps the WAL short, this caps it near
; 256 MB if the helper can't keep up (CPU saturated for a long time).
DEFSTR sql_pragmas, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=65536; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000; SELECT count(*) FROM users WHERE id = 1;",0
DEFSTR sql_ckpt, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;",0
DEFSTR arg_ckpt, "--ckpt",0
DEFSTR arg_server, "server",0
DEFSTR self_exe, "/proc/self/exe",0
DEFSTR s_nouring, "io_uring setup failed (needs Linux 6.1+ with io_uring enabled)",10

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
wait_ts:  dq 0, 6000000        ; __kernel_timespec
wait_arg: dq 0, 0, wait_ts     ; io_uring_getevents_arg: sigmask, sigmask_sz + pad, ts

section .bss
alignb 16
rings:    resb MAXRINGS*128
arena:    resb BATCH_OUT
segs:     resb MAXSEG*16        ; {who q, off d, len d}
deferred: resq MAXCQE
txtbuf:   resb MAXREQ           ; decoded body
unamebuf: resb 1024
numbuf:   resb 64

; HMAC key schedule: sha state (h[8]=32, n=8, b[64], bl=4) = 108 bytes, pad to 112
align 16
hm_in:    resb 112
hm_out:   resb 112
b64tab:   resb 256

alignb 8
db_h:     resq 1
st_feed:  resq 1
st_post:  resq 1
st_fids:  resq 1
st_flikes: resq 1
st_frows: resq 1
st_ins:   resq 1
st_like:  resq 1
st_exists: resq 1
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
lfd:      resd 1
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
  mov rdi, rax
  mov rsi, [w_pl+24]
  call parse_sint
  mov rcx, [now_s]
  cmp rcx, rax
  jge .bad
  mov rax, [w_pl+48]
  test rax, rax
  jz .no_nbf
  mov rdi, rax
  mov rsi, [w_pl+56]
  call parse_sint
  mov rcx, [now_s]
  cmp rcx, rax
  jl .bad
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

; parse_sint: rdi=ptr, rsi=end -> rax = signed integer (integer part only)
parse_sint:
  xor rax, rax
  xor r9, r9                 ; neg flag
  cmp rdi, rsi
  jae .done
  cmp byte [rdi], '-'
  jne .digs
  mov r9, 1
  inc rdi
.digs:
  cmp rdi, rsi
  jae .fin
  movzx edx, byte [rdi]
  cmp dl, '0'
  jb .fin
  cmp dl, '9'
  ja .fin
  imul rax, rax, 10
  sub dl, '0'
  movzx edx, dl
  add rax, rdx
  inc rdi
  jmp .digs
.fin:
  test r9, r9
  jz .done
  neg rax
.done:
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

section .bss
alignb 8
fr_ids:   resq 20
fr_likes: resq 20
fr_at:    resd 20
fr_len:   resd 20
fr_k:     resq 1
fr_slot:  resb 256
fr_tmp:   resb BATCH_OUT
section .text

; feed_range: edi=close -> eax 1 feed written, 0 fall back to the reference query, -1 database error.
; The newest 20 ids usually sit in a narrow id range (new posts get consecutive ids). Then one scan of
; that range in likes and one in posts replace the 20 per-row count subqueries and join lookups; the
; rows are put back in feed order. Ids spread over 256 or more, or a row missing: the reference query.
; regs: ebx=n r12=lo r13=hi, later the rows start r14=arena mark r15d=close, later rows found
feed_range:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r15d, edi
  xor ebx, ebx
.ids:
  mov rdi, [st_fids]
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .idsdone
  mov rdi, [st_fids]
  xor esi, esi
  call sqlite3_column_int64
  mov [fr_ids+rbx*8], rax
  inc ebx
  jmp .ids
.idsdone:
  mov rdi, [st_fids]
  call sqlite3_reset
  test eax, eax
  jnz .dberr
  xor r12d, r12d            ; lo = hi = 0 for an empty table
  xor r13d, r13d
  test ebx, ebx
  jz .span
  mov r12, [fr_ids]
  mov r13, r12
  xor ecx, ecx
.minmax:
  inc ecx
  cmp ecx, ebx
  jae .span
  mov rax, [fr_ids+rcx*8]
  cmp rax, r12
  cmovl r12, rax
  cmp rax, r13
  cmovg r13, rax
  jmp .minmax
.span:
  mov rax, r13
  sub rax, r12
  cmp rax, 256
  jae .fallback
  lea rdi, [fr_slot]        ; slot[id - lo] = feed position, -1 if not in the feed
  mov ecx, 256
  mov al, -1
  rep stosb
  lea rdi, [fr_likes]
  mov ecx, 20
  xor eax, eax
  rep stosq
  xor ecx, ecx
.slots:
  cmp ecx, ebx
  jae .likes
  mov rax, [fr_ids+rcx*8]
  sub rax, r12
  mov [fr_slot+rax], cl
  inc ecx
  jmp .slots
.likes:
  mov rdi, [st_flikes]
  mov esi, 1
  mov rdx, r12
  call sqlite3_bind_int64
  mov rdi, [st_flikes]
  mov esi, 2
  mov rdx, r13
  call sqlite3_bind_int64
.lrow:
  mov rdi, [st_flikes]
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .ldone
  mov rdi, [st_flikes]
  xor esi, esi
  call sqlite3_column_int64
  sub rax, r12
  movsx rcx, byte [fr_slot+rax]
  test rcx, rcx
  js .lrow
  inc qword [fr_likes+rcx*8]
  jmp .lrow
.ldone:
  mov rdi, [st_flikes]
  call sqlite3_reset
  test eax, eax
  jnz .dberr
  mov rdi, [st_frows]
  mov esi, 1
  mov rdx, r12
  call sqlite3_bind_int64
  mov rdi, [st_frows]
  mov esi, 2
  mov rdx, r13
  call sqlite3_bind_int64
  mov r14, [awr]
  mov edi, 200
  mov esi, r15d
  call begin_resp
  PUTL j_postsarr
  mov r13, [awr]            ; rows start
  xor r15d, r15d            ; rows found
.row:
  mov rdi, [st_frows]
  call sqlite3_step
  cmp eax, SQLITE_ROW
  jne .rdone
  mov rdi, [st_frows]
  xor esi, esi
  call sqlite3_column_int64
  sub rax, r12
  movsx rcx, byte [fr_slot+rax]
  test rcx, rcx
  js .row
  mov [fr_k], rcx
  mov rax, [awr]
  sub rax, r13
  mov [fr_at+rcx*4], eax
  mov rdi, [st_frows]
  mov rsi, [fr_likes+rcx*8]
  call put_post_row
  mov rcx, [fr_k]
  mov rax, [awr]
  sub rax, r13
  sub eax, [fr_at+rcx*4]
  mov [fr_len+rcx*4], eax
  inc r15d
  jmp .row
.rdone:
  mov rdi, [st_frows]
  call sqlite3_reset
  test eax, eax
  jnz .rowerr
  cmp r15d, ebx
  jne .missing
  mov rcx, [awr]            ; copy the rows out, then back in feed order
  sub rcx, r13
  lea rdi, [fr_tmp]
  mov rsi, r13
  rep movsb
  mov [awr], r13
  xor r15d, r15d
.order:
  cmp r15d, ebx
  jae .odone
  test r15d, r15d
  jz .nocomma
  mov dil, ','
  call a_char
.nocomma:
  mov eax, [fr_at+r15*4]
  lea rsi, [fr_tmp+rax]
  mov edx, [fr_len+r15*4]
  call a_emit
  inc r15d
  jmp .order
.odone:
  PUTL j_closearr
  call finish
  mov eax, 1
  jmp .ret
.missing:
  mov [awr], r14
.fallback:
  xor eax, eax
  jmp .ret
.rowerr:
  mov [awr], r14
.dberr:
  mov eax, -1
.ret:
  pop r15
  pop r14
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

; txn: open the batch transaction if needed -> rax 1/0
txn:
  cmp dword [in_txn], 0
  jne .yes
  sub rsp, 8                ; keep rsp 16-byte aligned at every call into SQLite
  mov rdi, [st_begin]
  call step_reset
  add rsp, 8
  test eax, eax
  jnz .no
  mov dword [in_txn], 1
.yes:
  mov eax, 1
  ret
.no:
  mov dword [in_txn], 0
  xor eax, eax
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

; db_fail
db_fail:
  cmp dword [in_txn], 0
  je .r
  sub rsp, 8
  mov rdi, [db_h]
  call sqlite3_get_autocommit
  add rsp, 8
  test eax, eax
  jz .r
  mov dword [in_txn], 0
  cmp dword [txn_writes], 0
  je .nw
  mov dword [batch_failed], 1
.nw:
  mov dword [txn_writes], 0
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
  ; method flags
  mov rax, [g_ml]
  cmp rax, 3
  jne .notget
  mov rsi, [g_m]
  cmp byte [rsi], 'G'
  jne .notget
  cmp byte [rsi+1], 'E'
  jne .notget
  cmp byte [rsi+2], 'T'
  jne .notget
  mov byte [.is_get], 1
  jmp .methdone
.notget:
  mov byte [.is_get], 0
.methdone:
  mov rax, [g_ml]
  cmp rax, 4
  jne .notpost
  mov rsi, [g_m]
  cmp dword [rsi], 'POST'
  jne .notpost
  mov byte [.is_post], 1
  jmp .pdone
.notpost:
  mov byte [.is_post], 0
.pdone:
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
  ; feed
  call txn
  test rax, rax
  jz .e500
  movzx edi, r15b
  call feed_range
  test eax, eax
  jz .feed_ref
  jns .ret
  call db_fail
  jmp .e500
.feed_ref:
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
  ; seg = path+7, sl = memchr(seg,'/',pl-7)
  lea r13, [rbx+7]          ; seg
  mov rdi, r13
  lea rsi, [rbx]
  add rsi, r12              ; path+pl
  mov dl, '/'
  call memchr_
  mov r14, rax              ; sl (0 if none)
  ; GET /posts/:id  (no slash)
  cmp byte [.is_get], 0
  je .chk_like
  test r14, r14
  jnz .chk_like
  mov rdi, r13
  mov rsi, rbx
  add rsi, r12
  sub rsi, r13              ; pl-7 length
  call parse_id
  cmp rax, 0
  jl .e_badid
  mov [.idv], rax
  call txn
  test rax, rax
  jz .e500
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
  ; POST /posts/:id/like : is_post, sl, (path+pl - sl)==5, sl=="/like"
  cmp byte [.is_post], 0
  je .chk_posts_exact
  test r14, r14
  jz .chk_posts_exact
  mov rax, rbx
  add rax, r12
  sub rax, r14             ; path+pl - sl
  cmp rax, 5
  jne .chk_posts_exact
  mov rdi, r14
  lea rsi, [m_like]
  mov rcx, 5
  call memeq_n
  jnz .chk_posts_exact
  ; auth first
  mov rdi, [g_authh]
  mov rsi, [g_al]
  call auth
  test eax, eax
  jnz .auth_err
  ; id = parse_id(seg, sl-seg)
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
  mov rdi, [db_h]
  call sqlite3_changes
  mov [.created], eax
  test eax, eax
  jnz .like_resp
  ; not created -> check existence
  mov rdi, [st_exists]
  mov esi, 1
  mov rdx, [.idv]
  call sqlite3_bind_int64
  mov rdi, [st_exists]
  call sqlite3_step
  mov [.rc], eax
  mov rdi, [st_exists]
  call sqlite3_reset
  cmp dword [.rc], SQLITE_DONE
  jne .like_exists_ok
  ERR 404, e_nopost
.like_exists_ok:
  cmp dword [.rc], SQLITE_ROW
  je .like_resp
  jmp .like_500
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
  ; jdoc(body, bl, w_body, 1)
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
  ; decode into txtbuf
  mov rax, [w_body+16]
  mov [.js], rax
  mov rax, [w_body+24]
  mov [.je], rax
  lea rdi, [.js]
  lea rsi, [txtbuf]
  mov rdx, MAXREQ
  call jstr
  cmp rax, 0
  jl .e_badjson
  ; trim
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
  ; count code points
  xor rcx, rcx              ; chars
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
  mov rdi, [st_ins]
  mov esi, 2
  mov rdx, r13
  mov rcx, r14
  sub rcx, r13
  xor r8, r8               ; SQLITE_STATIC
  mov r9, r13
  mov [.bodyp], r13
  mov [.bodye], r14
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
  mov r13, [awr]           ; mark
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
  mov rdi, [st_ins]          ; run the INSERT ... RETURNING to completion
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
  call step_reset
  test eax, eax
  jnz .health_bad
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
  mov rdi, [db_h]
  call sqlite3_errmsg
  mov rsi, rax
  ; strlen
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

; memmem4: rdi=p, rsi=end, looks for CR LF CR LF -> rax=ptr to first CR or 0
memmem_crlf2:
  lea rcx, [rsi-3]
.l:
  cmp rdi, rcx
  ja .no
  cmp dword [rdi], 0x0a0d0a0d   ; little-endian "\r\n\r\n" = 0d 0a 0d 0a
  je .yes
  inc rdi
  jmp .l
.yes:
  mov rax, rdi
  ret
.no:
  xor rax, rax
  ret

; serve: rsi=buf, rdx=n -> rax=used or -1 ; sets closing_flag
; stack locals: clen, authh, al, close, expect, he, le, body
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
  ; arena guard: the rest waits for the next batch
  cmp dword [nsegs], MAXSEG
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
  ; he = memmem_crlf2(p,e)
  mov rdi, r14
  mov rsi, r15
  call memmem_crlf2
  test rax, rax
  jnz .havehdr
  ; no full header
  mov rax, r15
  sub rax, r14
  cmp rax, MAXREQ
  ja .fail
  jmp .done
.havehdr:
  mov [rsp+8], rax          ; he (points at first \r of the \r\n\r\n)
  ; le = memchr(p,'\r', he+2-p)
  mov rdi, r14
  mov rsi, [rsp+8]
  add rsi, 2
  mov dl, 13
  call memchr_
  mov [rsp+16], rax         ; le
  ; sp1 = memchr(m,' ', le-m)
  mov rdi, r14
  mov rsi, [rsp+16]
  mov dl, ' '
  call memchr_
  test rax, rax
  jz .fail
  mov r8, rax               ; sp1
  mov [rsp+24], r8
  ; path = sp1+1 ; sp2 = memchr(path,' ', le-path)
  lea rdi, [r8+1]
  mov rsi, [rsp+16]
  mov dl, ' '
  call memchr_
  test rax, rax
  jz .fail
  mov [rsp+32], rax         ; sp2
  ; http10
  mov rax, [rsp+16]         ; le
  mov rcx, [rsp+32]         ; sp2
  sub rax, rcx
  dec rax                   ; le-sp2-1
  mov dword [rsp+44], 0     ; close
  cmp rax, 8
  jne .notv10
  mov rsi, [rsp+32]
  add rsi, 1
  ; compare "HTTP/1.0"
  mov rax, [rsi]
  mov rcx, 0x302e312f50545448    ; "HTTP/1.0" little-endian
  cmp rax, rcx
  jne .notv10
  mov dword [rsp+44], 1     ; close=1
.notv10:
  mov qword [rsp+48], 0     ; authh
  mov qword [rsp+56], 0     ; al
  mov qword [rsp+64], 0     ; clen
  mov dword [rsp+72], 0     ; expect
  ; header loop h = le+2 ; while h < he+2
  mov r14, [rsp+16]
  add r14, 2                ; h
.hloop:
  mov rax, [rsp+8]
  add rax, 2                ; he+2
  cmp r14, rax
  jae .hdone
  ; hl = memchr(h,'\r', he+2-h)
  mov rdi, r14
  mov rax, [rsp+8]
  lea rsi, [rax+2]
  mov dl, 13
  call memchr_
  mov r15, rax              ; hl
  ; c = memchr(h,':', hl-h)
  mov rdi, r14
  mov rsi, r15
  mov dl, ':'
  call memchr_
  test rax, rax
  jz .hnext
  mov r8, rax               ; c
  ; v = c+1 skip spaces/tabs
  lea r9, [r8+1]
.skipv:
  cmp r9, r15
  jae .vdone
  movzx eax, byte [r9]
  cmp al, ' '
  je .skv
  cmp al, 9
  je .skv
  jmp .vdone
.skv:
  inc r9
  jmp .skipv
.vdone:
  ; ve = hl trim trailing ws
  mov r10, r15
.trimve:
  cmp r10, r9
  jbe .vetrimdone
  movzx eax, byte [r10-1]
  cmp al, ' '
  je .vd
  cmp al, 9
  je .vd
  jmp .vetrimdone
.vd:
  dec r10
  jmp .trimve
.vetrimdone:
  ; kn = c-h
  mov rcx, r8
  sub rcx, r14              ; kn
  ; content-length?
  cmp rcx, 14
  jne .not_cl
  mov rdi, r14
  lea rsi, [h_contentlength]
  push rcx
  call memeq_ci
  pop rcx
  jnz .not_cl
  ; parse clen digits from r9..r10
  xor rax, rax
  mov rsi, r9
.cldig:
  cmp rsi, r10
  jae .clset
  movzx edx, byte [rsi]
  cmp dl, '0'
  jb .fail
  cmp dl, '9'
  ja .fail
  imul rax, rax, 10
  sub dl, '0'
  movzx edx, dl
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
  push rcx
  call memeq_ci
  pop rcx
  jnz .not_auth
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
  push rcx
  call memeq_ci
  pop rcx
  jnz .not_conn
  mov rax, r10
  sub rax, r9              ; vlen
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
  push rcx
  call memeq_ci
  pop rcx
  jnz .not_expect
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
  push rcx
  call memeq_ci
  pop rcx
  jnz .hnext
  jmp .fail               ; transfer-encoding not supported
.hnext:
  lea r14, [r15+2]        ; h = hl+2
  jmp .hloop
.hdone:
  ; body = he+4
  mov rax, [rsp+8]
  add rax, 4
  mov [rsp+80], rax        ; body
  ; if (e - body < clen)
  lea rcx, [rbx+r12]       ; e
  mov rdx, rcx
  sub rdx, rax             ; e-body
  cmp rdx, [rsp+64]
  jae .complete
  ; incomplete: maybe 100-continue
  cmp dword [rsp+72], 0
  je .done
  cmp rax, rcx             ; body==e ?
  jne .done
  PUTL s_100cont
  jmp .done
.complete:
  ; set globals and call handle; m = p = buf+used
  lea r8, [rbx+r13]
  mov [g_m], r8
  mov rax, [rsp+24]        ; sp1
  sub rax, r8
  mov [g_ml], rax
  mov rax, [rsp+24]
  inc rax                  ; path
  mov [g_path], rax
  mov rcx, [rsp+32]        ; sp2
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
  mov eax, dword [rsp+44]
  test eax, eax
  jz .nocl
  mov dword [closing_flag], 1
.nocl:
  ; used = body+clen - buf
  mov rax, [rsp+80]
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
  mov rdi, [db_h]
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
  mov eax, SYS_exit_group
  syscall

; db_open: rdi=path
db_open:
  push rbx
  sub rsp, 16
  lea rsi, [db_h]
  mov edx, SQLITE_OPEN_RW
  xor rcx, rcx
  call sqlite3_open_v2
  test eax, eax
  jnz .fatal
  mov rdi, [db_h]
  mov esi, 5000
  call sqlite3_busy_timeout
  mov rdi, [db_h]
  lea rsi, [sql_pragmas]
  xor rdx, rdx
  xor rcx, rcx
  xor r8, r8
  call sqlite3_exec
  test eax, eax
  jnz .fatal
  lea rdi, [sql_feed]
  call prep
  mov [st_feed], rax
  lea rdi, [sql_post]
  call prep
  mov [st_post], rax
  lea rdi, [sql_fids]
  call prep
  mov [st_fids], rax
  lea rdi, [sql_flikes]
  call prep
  mov [st_flikes], rax
  lea rdi, [sql_frows]
  call prep
  mov [st_frows], rax
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
  lea rdi, [sql_rollback]
  call prep
  mov [st_rollback], rax
  mov rdi, [st_feed]         ; touch the hot pages before the first request
  call step_reset
  add rsp, 16
  pop rbx
  ret
.fatal:
  mov edi, 1
  mov eax, SYS_exit_group
  syscall

; parse_ipv4: rdi=string "a.b.c.d" -> eax addr in memory order (a at sockaddr+4)
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
  mov [r11+r9], al
  inc r9
  xor eax, eax
  inc rdi
  jmp .l
.end:
  mov [r11+r9], al
  mov eax, dword [sockaddr+4]
  ret

; getenv: rdi=name, rsi=namelen -> rax=value ptr or 0
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
  ; compare name
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

section .bss
alignb 16
rlbuf:    resb 16
actbuf:   resb 32
params:   resb 120               ; struct io_uring_params
rsrc:     resb 32                ; struct io_uring_rsrc_register
bufreg:   resb 40                ; struct io_uring_buf_reg
evval:    resq 1
efd:      resd 1
nrings:   resd 1
ring_cap: resd 1                 ; rings we may still add (lowered if a setup fails)
tsize:    resd 1
naccepts: resd 1
nrearm:   resd 1
rearm:    resq MAXCQE
child_argv: resq 4
ck_log:   resd 1
ck_done:  resd 1
ck_ts:    resq 2
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
  mov edx, esi
  mov esi, eax
  mov edi, [rdi+R_FD]
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
  shl rdx, 11               ; * BSZ
  add rdx, [rdi+R_BUFS]
  mov [r8+rcx], rdx
  mov dword [r8+rcx+8], BSZ
  mov [r8+rcx+12], si
  inc eax
  mov [r8+14], ax
  ret

; arm_evfd: extra rings signal this eventfd; ring 0 reads it, so its one wait covers all of them
arm_evfd:
  sub rsp, 8
  xor edi, edi
  call get_sqe
  mov byte [rax], OP_READ
  mov ecx, [efd]
  mov [rax+4], ecx
  lea rcx, [evval]
  mov [rax+16], rcx
  mov dword [rax+24], 8
  mov qword [rax+32], T_EVFD
  add rsp, 8
  ret

; ring_init: edi=k -> eax 0 ok, -1 failed. Accepted sockets live in each ring's registered file
; table, not in the process descriptor table. A table can't be larger than RLIMIT_NOFILE, so more
; rings are added as tables fill up. Ring 0 is the one the loop waits on, so its completions run in
; that wait (DEFER_TASKRUN).
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
  test r12d, r12d
  jnz .flags
  or dword [params+8], SETUP_DEFER_TASKRUN
.flags:
  mov edi, SQ_ENTRIES
  lea rsi, [params]
  mov eax, SYS_io_uring_setup
  syscall
  test eax, eax
  js .fail0
  mov [rbx+R_FD], eax
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
  mov eax, [tsize]                  ; sparse registered file table, tsize slots
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
  mov [bufreg], rax
  mov dword [bufreg+8], NBUF
  mov esi, REG_PBUF_RING
  lea rdx, [bufreg]
  mov r10d, 1
  call .reg
  mov edi, [tsize]
  shl rdi, 6
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx+R_CONNS], rax
  mov edi, NBUF*BSZ
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx+R_BUFS], rax
  xor r13d, r13d
.bufs:
  mov rdi, rbx
  mov esi, r13d
  call buf_put
  inc r13d
  cmp r13d, NBUF
  jb .bufs
  test r12d, r12d
  jz .ok
  cmp dword [efd], 0
  jge .haveefd
  xor edi, edi
  mov esi, 0x80800                  ; EFD_CLOEXEC | EFD_NONBLOCK
  mov eax, SYS_eventfd2
  syscall
  test eax, eax
  js .fail
  mov [efd], eax
  call arm_evfd
.haveefd:
  mov esi, REG_EVENTFD
  lea rdx, [efd]
  mov r10d, 1
  call .reg
.ok:
  lea eax, [r12+1]
  mov [nrings], eax
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
  ; ponytail: a failed ring leaks its mappings once; no further rings are tried after it
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

; conn_close: rdi=who. Ends the pending receive and send, then frees the slot (one linked chain). A
; send still in flight frees its own buffer when it completes; the generation bump makes late
; completions harmless.
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
  call sqlite3_free
  mov rdi, [r13+C_Q]
  call sqlite3_free
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
  mov [rax+8], r14
  mov qword [rax+32], T_NOP
  test r15, r15
  jz .close
  mov edi, ebx
  and edi, 31
  call get_sqe
  mov byte [rax], OP_ASYNC_CANCEL
  mov byte [rax+1], SQE_IO_HARDLINK
  mov dword [rax+4], -1
  mov [rax+8], r15
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
  mov dword [closing_flag], 0
  mov dword [arena_full], 0
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
  call sqlite3_malloc64
  test rax, rax
  jz .close
  mov [r12+C_IN], rax
.move:
  mov rdi, [r12+C_IN]
  mov rsi, r13
  add rsi, [rsp]
  mov rcx, [rsp+8]
  rep movsb
  mov rcx, [rsp+8]
  mov [r12+C_INLEN], ecx
  cmp dword [arena_full], 0
  je .ret
  mov eax, [ndef]
  mov [deferred+rax*8], rbx
  inc dword [ndef]
  jmp .ret
.noleft:
  mov rdi, [r12+C_IN]
  test rdi, rdi
  jz .chkclose
  call sqlite3_free
  mov qword [r12+C_IN], 0
  mov qword [r12+C_INLEN], 0        ; and C_INCAP
.chkclose:
  cmp byte [closing_flag], 0
  je .ret
  cmp [awr], r15
  jne .ret
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

; on_data: rdi=who, rsi=conn, rdx=bytes, ecx=n
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
  cmp dword [r12+C_INLEN], 0
  jne .append
  cmp byte [r12+C_SENDING], 0
  jne .append
  call process
  jmp .ret
.append:                    ; a partial request, or input that arrives while a send is in flight
  mov eax, [r12+C_INCAP]
  sub eax, [r12+C_INLEN]
  cmp eax, r14d
  jae .fits
  mov esi, [r12+C_INLEN]
  add esi, r14d
  add esi, esi
  mov [r12+C_INCAP], esi
  mov rdi, [r12+C_IN]
  call sqlite3_realloc64
  test rax, rax
  jz .close
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

; send_bytes: rdi=who, rsi=conn, rdx=bytes, ecx=n. One send in flight per connection keeps responses
; in order; later ones queue behind it. Each send owns a copy, so the arena is reused at once.
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
  lea rdi, [r14+S_D]
  call sqlite3_malloc64
  test rax, rax
  jz .close
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
  call sqlite3_realloc64
  test rax, rax
  jz .close
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
  jmp .ret
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
  jz .free
  test r13d, r13d
  jle .free
  add [rbx+S_OFF], r13d
  mov eax, [rbx+S_OFF]
  cmp eax, [rbx+S_LEN]
  jae .free
  mov rdi, rbx              ; short send: the rest
  call send_op
  jmp .ret
.free:
  mov rdi, rbx
  call sqlite3_free
  test r12, r12
  jz .ret
  test r13d, r13d
  jle .close
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
  jz .ret
  mov rdi, r14              ; input that arrived while sending
  mov rsi, r12
  mov rdx, [r12+C_IN]
  call process
  jmp .ret
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

; arm_accepts: single-shot accepts, a few in flight, on the first ring with room. An accept that finds
; its table full has already taken the connection and can only reset it, so tables are never filled
; to the last 16 slots (closes still in flight may hold a few), and a multishot accept can't be stopped
; in time.
arm_accepts:
  push rbx
  push r12
  push r13
  xor ebx, ebx
.l:
  cmp dword [naccepts], ACCEPTS
  jae .done
  cmp ebx, [ring_cap]
  jae .done
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
  mov ecx, [lfd]
  mov [rax+4], ecx
  mov dword [rax+44], FILE_INDEX_ALLOC
  mov qword [rax+32], T_ACCEPT
  inc dword [r12+R_PEND]
  inc dword [naccepts]
  jmp .l
.done:
  pop r13
  pop r12
  pop rbx
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
  cmp eax, T_EVFD
  jne .l
  call arm_evfd
  jmp .l
.send:
  call send_done
  jmp .l
.accept:
  dec dword [rbx+R_PEND]
  dec dword [naccepts]
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
  call conn_of
  test byte [rsp+8], CQE_F_BUFFER
  jz .nobuf
  test rax, rax
  jz .putbuf
  cmp qword [rsp], 0
  jle .putbuf
  mov rdi, r15
  mov rsi, rax
  mov edx, [rsp+8]
  shr edx, 16
  shl rdx, 11
  add rdx, [rbx+R_BUFS]
  mov ecx, [rsp]
  call on_data
.putbuf:
  mov rdi, rbx
  mov esi, [rsp+8]
  shr esi, 16
  call buf_put
.nobuf:
  test byte [rsp+8], CQE_F_MORE
  jnz .l
  mov rdi, r15
  call conn_of
  test rax, rax
  jz .l
  mov rcx, [rsp]
  cmp rcx, 0
  jg .rearm
  cmp rcx, -105             ; ENOBUFS: rearmed once buffers are back
  je .rearm
  cmp byte [rax+C_SENDING], 0
  je .close
  mov byte [rax+C_CLOSING], 1   ; EOF: finish the reply in flight first
  jmp .l
.close:
  mov rdi, r15
  call conn_close
  jmp .l
.rearm:
  mov eax, [nrearm]
  mov [rearm+rax*8], r15
  inc dword [nrearm]
  jmp .l
.done:
  mov eax, [rsp+12]
  add rsp, 16
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; Checkpoints copy the WAL back into the database and fsync it. Run inline (SQLite's auto-checkpoint)
; they stall every client in the batch for the length of the fsync, so this helper process does them
; and the event loop never waits on the disk. rbx = SQLITE_PATH; reached by jmp from main, never returns.
checkpointer:
  mov edi, 1                ; PR_SET_PDEATHSIG
  mov esi, 15               ; SIGTERM
  mov eax, 157              ; prctl
  syscall
  call sqlite3_initialize   ; built with SQLITE_OMIT_AUTOINIT
  mov rdi, rbx
  lea rsi, [db_h]
  mov edx, SQLITE_OPEN_RW
  xor ecx, ecx
  call sqlite3_open_v2
  test eax, eax
  jnz .fatal
  mov rdi, [db_h]
  mov esi, 100
  call sqlite3_busy_timeout
  mov rdi, [db_h]
  lea rsi, [sql_ckpt]
  xor edx, edx
  xor ecx, ecx
  xor r8d, r8d
  call sqlite3_exec
.loop:
  mov dword [ck_log], 0
  mov dword [ck_done], 0
  mov rdi, [db_h]
  xor esi, esi              ; every attached database
  xor edx, edx              ; SQLITE_CHECKPOINT_PASSIVE
  lea rcx, [ck_log]
  lea r8, [ck_done]
  call sqlite3_wal_checkpoint_v2
  mov qword [ck_ts+8], 10000000   ; idle: 10 ms
  mov eax, dword [ck_log]
  test eax, eax
  jle .sleep
  cmp dword [ck_done], eax
  jge .sleep
  mov qword [ck_ts+8], 1000000    ; still behind: 1 ms
.sleep:
  lea rdi, [ck_ts]
  xor esi, esi
  mov eax, SYS_nanosleep
  syscall
  jmp .loop
.fatal:
  mov edi, 1
  mov eax, SYS_exit_group
  syscall

; run loop: one io_uring_enter per iteration submits everything queued and waits on ring 0
run:                        ; reached by jmp from main, never returns
  xor r15d, r15d            ; completions seen by the last pass
.loop:
  xor r12d, r12d
.rearm:
  cmp r12d, [nrearm]
  jae .rearmed
  mov rdi, [rearm+r12*8]
  call conn_of
  test rax, rax
  jz .rnext
  mov rdi, [rearm+r12*8]
  call arm_recv
.rnext:
  inc r12d
  jmp .rearm
.rearmed:
  mov dword [nrearm], 0
  call arm_accepts
  mov r12d, 1
.extra:                     ; rings 1.. complete in the background: submit, flush overflow
  cmp r12d, [nrings]
  jae .wait
  mov eax, r12d
  shl eax, 7
  lea rdi, [rings+rax]
  mov rax, [rdi+R_SQHEAD]
  mov eax, [rax]
  cmp eax, [rdi+R_TAIL]
  jne .enter
  mov rax, [rdi+R_SQFLAGS]
  test dword [rax], SQ_CQ_OVERFLOW
  jz .enext
.enter:
  xor esi, esi
  mov edx, ENTER_GETEVENTS
  call ring_enter
.enext:
  inc r12d
  jmp .extra
.wait:
  ; Busy (the last pass saw 4+ completions): wait for up to 128 or 6 ms, so a batch, its commit and
  ; its sends cover more requests per wake-up. The target is capped at 40% of open connections so a
  ; few clients without think time never sit out the timeout.
  cmp r15d, 4
  jae .busy
  lea rdi, [rings]
  mov esi, 1
  mov edx, ENTER_GETEVENTS
  call ring_enter
  jmp .entered
.busy:
  xor eax, eax
  xor ecx, ecx
.open:
  mov edx, ecx
  shl edx, 7
  add eax, [rings+rdx+R_N]
  inc ecx
  cmp ecx, [nrings]
  jb .open
  add eax, eax
  xor edx, edx
  mov ecx, 5
  div ecx
  inc eax
  mov esi, 128
  cmp eax, esi
  cmovb esi, eax
  lea rdi, [rings]
  mov edx, ENTER_GETEVENTS | ENTER_EXT_ARG
  lea r8, [wait_arg]
  mov r9d, 24
  call ring_enter.arg
.entered:
  test rax, rax
  jns .woke
  cmp rax, -4               ; EINTR
  je .woke
  cmp rax, -16              ; EBUSY
  je .woke
  cmp rax, -62              ; ETIME: the busy wait timed out
  je .woke
  mov edi, 1
  mov eax, SYS_exit_group
  syscall
.woke:
  xor edi, edi
  mov eax, SYS_time
  syscall
  mov [now_s], rax
  xor r12d, r12d
  xor r15d, r15d
.drain:
  cmp r12d, [nrings]
  jae .flush
  mov edi, r12d
  call drain
  add r15d, eax
  inc r12d
  jmp .drain
.flush:
  call flush_batch
.defloop:                   ; requests held back by a full arena
  cmp dword [ndef], 0
  je .loop
  mov r12d, [ndef]
  mov dword [ndef], 0       ; re-deferrals land at indexes already read
  xor r13d, r13d
.dl:
  cmp r13d, r12d
  jae .dflush
  mov r14, [deferred+r13*8]
  mov rdi, r14
  call conn_of
  test rax, rax
  jz .dnext
  cmp dword [rax+C_INLEN], 0
  je .dnext
  cmp byte [rax+C_SENDING], 0
  jne .dnext
  mov rdi, r14
  mov rsi, rax
  mov rdx, [rax+C_IN]
  mov ecx, [rax+C_INLEN]
  call process
.dnext:
  inc r13d
  jmp .dl
.dflush:
  call flush_batch
  jmp .defloop

; main(edi=argc, rsi=argv, rdx=envp)
main:
  push rbx
  push r12
  push r13
  mov r12d, edi
  mov r13, rsi
  mov [envp_ptr], rdx
  lea rdi, [env_sqlite]
  mov rsi, env_sqlite_len
  call getenv_
  test rax, rax
  jz .missing
  mov rbx, rax
  cmp r12d, 2               ; --ckpt
  jne .notckpt
  mov rdi, [r13+8]
  lea rsi, [arg_ckpt]
  mov rcx, arg_ckpt_len     ; includes the NUL
  call memeq_n
  je checkpointer
.notckpt:
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
  mov qword [actbuf], 1     ; SIGCHLD ignored, so the checkpointer is reaped if it exits
  mov edi, 17
  lea rsi, [actbuf]
  xor edx, edx
  mov r10, 8
  mov eax, SYS_rt_sigaction
  syscall
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
  mov rax, [rlbuf]
  mov ecx, 1 << 20
  cmp rax, rcx
  cmova rax, rcx
  mov [tsize], eax
  mov dword [ring_cap], MAXRINGS
  mov dword [efd], -1
  ; b64 table
  lea rdi, [b64tab]
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
  call sqlite3_initialize
  xor edi, edi
  mov eax, SYS_time
  syscall
  mov [started], rax
  mov [now_s], rax
  mov qword [awr], arena
  mov rdi, rbx
  call db_open
  mov eax, 57               ; fork the checkpointer
  syscall
  test eax, eax
  jnz .listen               ; parent (or fork failed: the wal_autocheckpoint backstop remains)
  lea rax, [arg_server]
  mov [child_argv], rax
  lea rax, [arg_ckpt]
  mov [child_argv+8], rax
  lea rdi, [self_exe]
  lea rsi, [child_argv]
  mov rdx, [envp_ptr]
  mov eax, 59               ; execve
  syscall
  jmp .missing
.listen:
  call listen_start
  xor edi, edi
  call ring_init
  test eax, eax
  jz run
  mov edi, 2
  lea rsi, [s_nouring]
  mov edx, s_nouring_len
  mov eax, SYS_write
  syscall
.missing:
  mov edi, 1
  mov eax, SYS_exit_group
  syscall

; listen_start: blocking listener (io_uring waits on it), TCP_NODELAY inherited by accepted sockets
listen_start:
  mov edi, 2
  mov esi, 1 | 0x80000      ; SOCK_STREAM | SOCK_CLOEXEC
  xor edx, edx
  mov eax, SYS_socket
  syscall
  test rax, rax
  js .fatal
  mov dword [lfd], eax
  mov dword [one_val], 1
  mov edi, eax              ; SO_REUSEADDR
  mov esi, 1
  mov edx, 2
  lea r10, [one_val]
  mov r8, 4
  mov eax, SYS_setsockopt
  syscall
  mov edi, dword [lfd]      ; TCP_NODELAY
  mov esi, 6
  mov edx, 1
  lea r10, [one_val]
  mov r8, 4
  mov eax, SYS_setsockopt
  syscall
  mov word [sockaddr], 2    ; AF_INET
  mov eax, dword [g_port]
  xchg al, ah               ; htons
  mov word [sockaddr+2], ax
  mov rdi, [g_host]
  call parse_ipv4
  mov qword [sockaddr+8], 0
  mov edi, dword [lfd]
  lea rsi, [sockaddr]
  mov edx, 16
  mov eax, SYS_bind
  syscall
  test rax, rax
  js .fatal
  mov edi, dword [lfd]
  mov esi, 65535
  mov eax, SYS_listen
  syscall
  test rax, rax
  js .fatal
  ret
.fatal:
  mov edi, 1
  mov eax, SYS_exit_group
  syscall

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
