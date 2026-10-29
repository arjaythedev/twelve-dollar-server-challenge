; Feed API server in x86-64 assembly (NASM, System V ABI, Linux).
; All application logic is assembly: HTTP/1.1 parsing, routing, strict JSON validation + string
; decode, base64url, SHA-256/HMAC, JWT checks, JSON output, integer formatting, epoll loop (raw
; syscalls). The only external code is SQLite (pinned amalgamation, compiled in by build.sh).
; Design mirrors submissions/c-epoll-unorig-username/server.c: single process, level-triggered
; epoll, one deferred transaction per epoll batch, responses held until COMMIT, WAL checkpoints in
; a helper process (--ckpt).

bits 64
default rel

%define MAXFD        65536
%define RBUF         65536          ; read scratch
%define MAXREQ       65536          ; per-request header+body cap
%define INCAP        131072         ; per-connection carry buffer (leftover + one read)
%define BATCH_OUT    (4*1024*1024)  ; arena bytes held per loop iteration

; conn slot (32 bytes), indexed by fd
%define C_IN     0       ; q: input carry buffer ptr (0 if none)
%define C_OUT    8       ; q: partial-write buffer ptr (0 if none)
%define C_INLEN  16      ; d
%define C_OUTLEN 20      ; d
%define C_OUTOFF 24      ; d
%define C_OPEN   28      ; b
%define C_CLOSING 29     ; b
%define C_SIZE   32

; sqlite constants
%define SQLITE_ROW    100
%define SQLITE_DONE   101
%define SQLITE_BUSY   5
%define SQLITE_OPEN_RW 2
%define SQLITE_PREP_PERSISTENT 1

; syscalls
%define SYS_write 1
%define SYS_close 3
%define SYS_mmap 9
%define SYS_munmap 11
%define SYS_rt_sigaction 13
%define SYS_socket 41
%define SYS_bind 49
%define SYS_listen 50
%define SYS_setsockopt 54
%define SYS_nanosleep 35
%define SYS_exit_group 231
%define SYS_epoll_wait 232
%define SYS_epoll_ctl 233
%define SYS_epoll_create1 291
%define SYS_accept4 288
%define SYS_time 201
%define SYS_prlimit64 302

%define EPOLLIN 1
%define EPOLLOUT 4
%define EPOLLERR 8
%define EPOLLHUP 0x10
%define EPOLL_CTL_ADD 1
%define EPOLL_CTL_DEL 2
%define EPOLL_CTL_MOD 3

extern sqlite3_initialize, sqlite3_open_v2, sqlite3_busy_timeout, sqlite3_exec
extern sqlite3_prepare_v3, sqlite3_step, sqlite3_reset, sqlite3_bind_int64, sqlite3_bind_text
extern sqlite3_column_int64, sqlite3_column_text, sqlite3_column_bytes, sqlite3_changes
extern sqlite3_get_autocommit, sqlite3_errmsg, sqlite3_wal_checkpoint_v2

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
DEFSTR sql_beginimm, "BEGIN IMMEDIATE",0
DEFSTR sql_commit, "COMMIT",0
DEFSTR sql_rollback, "ROLLBACK",0
; wal_autocheckpoint is only a backstop: the --ckpt helper keeps the WAL short, this caps it near
; 256 MB if the helper can't keep up (CPU saturated for a long time).
DEFSTR sql_pragmas, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=65536; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000; SELECT count(*) FROM users WHERE id = 1;",0
DEFSTR sql_ckpt, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;",0
DEFSTR arg_shared, "--shared",0
DEFSTR arg_ckpt, "--ckpt",0
DEFSTR arg_server, "server",0
DEFSTR self_exe, "/proc/self/exe",0

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

section .bss
alignb 16
conns:    resb MAXFD*C_SIZE
arena:    resb BATCH_OUT
segs:     resb MAXFD*12         ; {fd d, off d, len d}
deferred: resd MAXFD
rbuf:     resb RBUF
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
st_beginimm: resq 1
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
nconns:   resd 1
epfd:     resd 1
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

; wstep: rdi=stmt -> rax=rc
wstep:
  push rbx
  mov rbx, rdi
  call sqlite3_step
  mov r11d, eax
  and r11d, 0xff
  cmp r11d, SQLITE_BUSY
  jne .fin
  cmp dword [txn_writes], 0
  jne .fin
  ; another worker committed since our snapshot: end the read part, retry under BEGIN IMMEDIATE
  mov rdi, rbx
  call sqlite3_reset
  cmp dword [in_txn], 0
  je .noend
  mov rdi, [st_commit]
  call step_reset
  mov dword [in_txn], 0
.noend:
  mov rdi, [st_beginimm]
  call step_reset
  mov r11d, eax
  test eax, eax
  jnz .ret_eax
  mov dword [in_txn], 1
  mov rdi, rbx
  call sqlite3_step
.fin:
  mov r11d, eax
  cmp r11d, SQLITE_ROW
  je .wset
  cmp r11d, SQLITE_DONE
  jne .ret_eax
.wset:
  mov dword [txn_writes], 1
.ret_eax:
  mov eax, r11d
  pop rbx
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

; serve: rdi=fd, rsi=buf, rdx=n -> rax=used or -1 ; sets closing_flag
; stack locals: clen, authh, al, close, expect, he, le, body
%define L_SIZE 96
serve:
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, L_SIZE
  mov [rsp+0], rdi          ; fd
  mov rbx, rsi              ; buf
  mov r12, rdx              ; n
  xor r13, r13              ; used
.loop:
  cmp r13, r12
  jae .done
  cmp dword [closing_flag], 0
  jne .done
  ; arena guard
  mov rax, [awr]
  lea rcx, [arena + (BATCH_OUT - 2*MAXREQ)]
  cmp rax, rcx
  jbe .arena_ok
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
  mov edi, dword [rsp+0]
  movsxd rdi, edi
  lea rsi, [s_100cont]
  mov edx, s_100cont_len
  mov eax, SYS_write
  syscall
  test rax, rax
  js .fail
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

section .bss
alignb 16
evbuf: resb 512*12
rlbuf: resb 16
actbuf: resb 32
ev_tmp: resb 16
section .text

; mmap_anon: rdi=size -> rax ptr (or <0 err)
mmap_anon:
  mov rsi, rdi
  xor rdi, rdi
  mov rdx, 3
  mov r10, 0x22
  mov r8, -1
  xor r9, r9
  mov eax, SYS_mmap
  syscall
  ret

; conn_close: rdi=fd
conn_close:
  push rbx
  push r12
  mov rbx, rdi
  shl rdi, 5
  lea r12, [conns+rdi]      ; slot
  mov rdi, [r12+C_IN]
  test rdi, rdi
  jz .noin
  mov rsi, INCAP
  mov eax, SYS_munmap
  syscall
.noin:
  mov rdi, [r12+C_OUT]
  test rdi, rdi
  jz .noout
  mov esi, dword [r12+C_OUTLEN]
  mov eax, SYS_munmap
  syscall
.noout:
  xor eax, eax
  mov [r12], rax            ; zero the whole 32-byte slot
  mov [r12+8], rax
  mov [r12+16], rax
  mov [r12+24], rax
  mov edi, ebx
  mov eax, SYS_close
  syscall
  dec dword [nconns]
  pop r12
  pop rbx
  ret

; watch (modify) / ep_add: rdi=fd, esi=events
watch:
  mov edx, EPOLL_CTL_MOD
  jmp ep_ctl
ep_add:
  mov edx, EPOLL_CTL_ADD
ep_ctl:
  mov dword [ev_tmp], esi
  mov [ev_tmp+4], rdi
  mov esi, edx
  mov edx, edi
  mov edi, dword [epfd]
  lea r10, [ev_tmp]
  mov eax, SYS_epoll_ctl
  syscall
  ret

; process: rdi=fd, rsi=buf, rdx=n
process:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov rbx, rdi             ; fd
  mov r12, rsi             ; buf
  mov r13, rdx             ; n
  mov r14, [awr]           ; start ptr
  mov dword [closing_flag], 0
  mov dword [arena_full], 0
  mov rdi, rbx
  mov rsi, r12
  mov rdx, r13
  call serve
  mov r15, rax             ; used
  test r15, r15
  jns .ok
  mov [awr], r14
  mov rdi, rbx
  call conn_close
  jmp .ret
.ok:
  ; seg if awr>start
  mov rax, [awr]
  cmp rax, r14
  jbe .noseg
  mov ecx, dword [nsegs]
  lea rdx, [segs]
  lea rdx, [rdx + rcx*4]
  lea rdx, [rdx + rcx*8]    ; segs + nsegs*12
  mov [rdx], ebx           ; fd
  mov r8, r14
  sub r8, arena
  mov [rdx+4], r8d         ; off
  mov r9, rax
  sub r9, r14
  mov [rdx+8], r9d         ; len
  inc dword [nsegs]
.noseg:
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]     ; slot
  mov cl, byte [closing_flag]
  mov [r10+C_CLOSING], cl
  ; left = closing?0:(n-used)
  xor rcx, rcx
  cmp byte [closing_flag], 0
  jne .haveleft
  mov rcx, r13
  sub rcx, r15             ; n-used
.haveleft:
  test rcx, rcx
  jz .nocopy
  ; need c->in; copy leftover [buf+used, +left)
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]
  mov rdi, [r10+C_IN]
  cmp rdi, r12
  je .inplace
  ; alloc if needed
  test rdi, rdi
  jnz .havebuf
  push rcx
  mov rdi, INCAP
  call mmap_anon
  pop rcx
  mov r11, rbx
  shl r11, 5
  lea r10, [conns+r11]
  mov [r10+C_IN], rax
  mov rdi, rax
.havebuf:
  ; copy from r12+used to c->in
  lea rsi, [r12+r15]
  push rcx
  rep movsb
  pop rcx
  jmp .setlen
.inplace:
  ; memmove c->in: src c->in+used, len rcx
  test r15, r15
  jz .setlen
  mov rdi, [r10+C_IN]
  lea rsi, [rdi+r15]
  push rcx
  rep movsb
  pop rcx
.setlen:
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]
  mov [r10+C_INLEN], ecx
  cmp dword [arena_full], 0
  je .ret
  mov eax, dword [ndef]
  lea rdx, [deferred]
  mov [rdx+rax*4], ebx
  inc dword [ndef]
  jmp .ret
.nocopy:
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]
  mov rdi, [r10+C_IN]
  test rdi, rdi
  jz .checkclose
  mov rsi, INCAP
  push r10
  mov eax, SYS_munmap
  syscall
  pop r10
  mov qword [r10+C_IN], 0
  mov dword [r10+C_INLEN], 0
.checkclose:
  cmp byte [closing_flag], 0
  je .ret
  mov rax, [awr]
  cmp rax, r14
  jne .ret
  mov rdi, rbx
  call conn_close
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; on_readable: rdi=fd
on_readable:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov rax, rbx
  shl rax, 5
  lea r12, [conns+rax]
  cmp qword [r12+C_OUT], 0
  jne .ret
  mov eax, dword [r12+C_INLEN]
  test eax, eax
  jnz .carry
  ; read into rbuf
  mov edi, ebx
  lea rsi, [rbuf]
  mov rdx, RBUF
  xor eax, eax             ; SYS_read=0
  syscall
  cmp rax, 0
  jle .readerr
  mov rdi, rbx
  lea rsi, [rbuf]
  mov rdx, rax
  call process
  jmp .ret
.carry:
  mov edi, ebx
  mov rsi, [r12+C_IN]
  mov eax, dword [r12+C_INLEN]
  add rsi, rax
  mov rdx, INCAP
  sub rdx, rax
  xor eax, eax
  syscall
  cmp rax, 0
  jle .readerr
  mov ecx, dword [r12+C_INLEN]
  add ecx, eax
  mov dword [r12+C_INLEN], ecx
  mov rdi, rbx
  mov rsi, [r12+C_IN]
  mov edx, ecx
  call process
  jmp .ret
.readerr:
  test rax, rax
  jz .close
  cmp rax, -11             ; EAGAIN
  je .ret
.close:
  mov rdi, rbx
  call conn_close
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; on_writable: rdi=fd
on_writable:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov rax, rbx
  shl rax, 5
  lea r12, [conns+rax]
  cmp qword [r12+C_OUT], 0
  je .ret
  mov edi, ebx
  mov rsi, [r12+C_OUT]
  mov eax, dword [r12+C_OUTOFF]
  add rsi, rax
  mov edx, dword [r12+C_OUTLEN]
  sub edx, eax
  mov eax, SYS_write
  syscall
  test rax, rax
  js .werr
  add dword [r12+C_OUTOFF], eax
  mov eax, dword [r12+C_OUTOFF]
  cmp eax, dword [r12+C_OUTLEN]
  jb .ret
  ; done
  mov rdi, [r12+C_OUT]
  mov esi, dword [r12+C_OUTLEN]
  mov eax, SYS_munmap
  syscall
  mov qword [r12+C_OUT], 0
  mov dword [r12+C_OUTLEN], 0
  mov dword [r12+C_OUTOFF], 0
  cmp byte [r12+C_CLOSING], 0
  je .cont
  mov rdi, rbx
  call conn_close
  jmp .ret
.cont:
  mov rdi, rbx
  mov esi, EPOLLIN
  call watch
  mov eax, dword [r12+C_INLEN]
  test eax, eax
  jz .ret
  mov rdi, rbx
  mov rsi, [r12+C_IN]
  mov edx, eax
  call process
  jmp .ret
.werr:
  cmp rax, -11
  je .ret
  mov rdi, rbx
  call conn_close
.ret:
  pop r13
  pop r12
  pop rbx
  ret

; flush_batch
flush_batch:
  push rbx
  push r12
  push r13
  push r14
  push r15
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
  xor r12, r12             ; i
.segloop:
  cmp r12d, dword [nsegs]
  jae .segdone
  lea rax, [segs]
  lea rax, [rax + r12*4]
  lea rax, [rax + r12*8]   ; seg i
  mov r13, rax
  mov ebx, dword [r13]     ; fd
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]
  cmp byte [r10+C_OPEN], 0
  je .segnext
  cmp dword [batch_failed], 0
  je .send
  mov rdi, rbx
  call conn_close
  jmp .segnext
.send:
  mov edi, ebx
  mov esi, dword [r13+4]   ; off
  lea rsi, [arena+rsi]
  mov edx, dword [r13+8]   ; len
  mov eax, SYS_write
  syscall
  test rax, rax
  jns .wrote
  cmp rax, -11
  je .partial0
  mov rdi, rbx
  call conn_close
  jmp .segnext
.partial0:
  xor rax, rax
.wrote:
  mov r8d, eax             ; w = bytes written
  mov edx, dword [r13+8]   ; len
  cmp r8d, edx
  jae .fullwrite
  ; partial: alloc out buffer, copy arena+off+w for (len-w) bytes
  mov r9d, edx
  sub r9d, r8d             ; remaining = len-w
  mov r14d, dword [r13+4]  ; off (r14 free here)
  push r8
  push r9
  mov edi, r9d              ; a segment (pipelined responses) can exceed any fixed cap
  call mmap_anon
  mov r15, rax             ; out buffer ptr
  pop r9
  pop r8
  mov rax, rbx
  shl rax, 5
  lea r10, [conns+rax]
  mov [r10+C_OUT], r15
  mov dword [r10+C_OUTLEN], r9d
  mov dword [r10+C_OUTOFF], 0
  ; copy
  mov rdi, r15
  lea rsi, [arena]
  add rsi, r14
  add rsi, r8              ; arena+off+w
  mov ecx, r9d
  rep movsb
  mov rdi, rbx
  mov esi, EPOLLOUT
  call watch
  jmp .segnext
.fullwrite:
  cmp byte [r10+C_CLOSING], 0
  je .segnext
  mov rdi, rbx
  call conn_close
  jmp .segnext
.segnext:
  inc r12
  jmp .segloop
.segdone:
  mov dword [nsegs], 0
  mov qword [awr], arena
  mov dword [batch_failed], 0
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
  lea rdi, [sql_beginimm]
  call prep
  mov [st_beginimm], rax
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

; listen_start: builds listening socket -> stores lfd, adds to epoll
listen_start:
  ; socket(AF_INET, SOCK_STREAM|NONBLOCK|CLOEXEC, 0)
  mov edi, 2
  mov esi, 1 | 0x800 | 0x80000
  xor edx, edx
  mov eax, SYS_socket
  syscall
  test rax, rax
  js .fatal
  mov dword [lfd], eax
  mov dword [one_val], 1
  ; SO_REUSEADDR=2
  mov edi, eax
  mov esi, 1
  mov edx, 2
  lea r10, [one_val]
  mov r8, 4
  mov eax, SYS_setsockopt
  syscall
  ; SO_REUSEPORT=15
  mov edi, dword [lfd]
  mov esi, 1
  mov edx, 15
  lea r10, [one_val]
  mov r8, 4
  mov eax, SYS_setsockopt
  syscall
  ; TCP_NODELAY: level 6, opt 1
  mov edi, dword [lfd]
  mov esi, 6
  mov edx, 1
  lea r10, [one_val]
  mov r8, 4
  mov eax, SYS_setsockopt
  syscall
  ; sockaddr_in
  mov word [sockaddr], 2          ; AF_INET
  mov eax, dword [g_port]
  xchg al, ah                     ; htons
  mov word [sockaddr+2], ax
  ; inet addr from g_host string
  mov rdi, [g_host]
  call parse_ipv4                 ; -> eax network-order addr
  mov dword [sockaddr+4], eax
  mov qword [sockaddr+8], 0
  ; bind
  mov edi, dword [lfd]
  lea rsi, [sockaddr]
  mov edx, 16
  mov eax, SYS_bind
  syscall
  test rax, rax
  js .fatal
  ; listen
  mov edi, dword [lfd]
  mov esi, 65535
  mov eax, SYS_listen
  syscall
  test rax, rax
  js .fatal
  movsxd rdi, dword [lfd]
  mov esi, EPOLLIN
  call ep_add
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
alignb 8
conn_hi:    resd 1
conn_lo:    resd 1
conn_max:   resd 1
spawned:    resd 1
spawn_pending: resd 1
ready_fd:   resd 1
notify_fd:  resd 1
pfd:        resd 2
fdarg:      resb 16
child_argv: resq 4
onebyte:    resb 8
ck_log:     resd 1
ck_done:    resd 1
ck_ts:      resq 2
section .text

; accept_conns: edi=limit. accepts while nconns < limit
accept_conns:
  push rbx
  mov ebx, edi
.acc:
  cmp dword [nconns], ebx
  jge .done
  mov edi, dword [lfd]
  xor esi, esi
  xor edx, edx
  mov r10d, 0x800 | 0x80000
  mov eax, SYS_accept4
  syscall
  test rax, rax
  js .done
  cmp rax, MAXFD
  jl .ok
  mov edi, eax
  mov eax, SYS_close
  syscall
  jmp .acc
.ok:
  mov rcx, rax
  shl rcx, 5
  lea r10, [conns+rcx]
  mov byte [r10+C_OPEN], 1
  inc dword [nconns]
  mov rdi, rax
  mov esi, EPOLLIN
  call ep_add
  jmp .acc
.done:
  pop rbx
  ret

; listen_stop: take what is queued, then close the listener so new clients go to the other worker
listen_stop:
  mov edi, dword [conn_max]
  call accept_conns
  mov edi, dword [lfd]
  mov eax, SYS_close
  syscall
  mov dword [lfd], -1
  ret

; exec_self: execve /proc/self/exe as "server" with child_argv[1..] (NULL-terminated), never returns
exec_self:
  lea rax, [arg_server]
  mov [child_argv], rax
  lea rdi, [self_exe]
  lea rsi, [child_argv]
  mov rdx, [envp_ptr]
  mov eax, 59               ; execve
  syscall
  mov edi, 1
  mov eax, SYS_exit_group
  syscall

; spawn_worker: fork+exec a copy of ourselves with --shared <notify fd>
spawn_worker:
  mov dword [spawn_pending], 0
  mov dword [spawned], 1
  lea rdi, [pfd]
  mov esi, 0x80000          ; O_CLOEXEC
  mov eax, 293              ; pipe2
  syscall
  test rax, rax
  js .fail
  mov eax, 57               ; fork
  syscall
  test rax, rax
  js .fail
  jnz .parent
  ; child: syscalls only until execve
  mov edi, 1                ; PR_SET_PDEATHSIG
  mov esi, 15               ; SIGTERM
  xor edx, edx
  xor r10, r10
  xor r8, r8
  mov eax, 157              ; prctl
  syscall
  mov edi, dword [pfd+4]
  mov esi, 2                ; F_SETFD
  xor edx, edx
  mov eax, 72               ; fcntl
  syscall
  ; fdarg = decimal(pfd[1])
  mov eax, dword [pfd+4]
  lea rdi, [fdarg+15]
  mov byte [rdi], 0
  mov ecx, 10
.dig:
  xor edx, edx
  div ecx
  add dl, '0'
  dec rdi
  mov [rdi], dl
  test eax, eax
  jnz .dig
  lea rax, [arg_shared]
  mov [child_argv+8], rax
  mov [child_argv+16], rdi
  mov qword [child_argv+24], 0
  jmp exec_self
.parent:
  mov edi, dword [pfd+4]
  mov eax, SYS_close
  syscall
  mov edi, dword [pfd]
  mov dword [ready_fd], edi
  movsxd rdi, edi
  mov esi, EPOLLIN
  call ep_add
.fail:
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

; run loop (never returns)
run:
  sub rsp, 8                ; 16-byte aligned for the calls below
  mov eax, SYS_epoll_create1
  mov edi, 0x80000          ; EPOLL_CLOEXEC
  syscall
  mov dword [epfd], eax
  call listen_start
  mov edi, dword [notify_fd]
  test edi, edi
  js .loop
  lea rsi, [arg_server]     ; any byte
  mov edx, 1
  mov eax, SYS_write
  syscall
  mov edi, dword [notify_fd]
  mov eax, SYS_close
  syscall
.loop:
  mov edi, dword [epfd]
  lea rsi, [evbuf]
  mov edx, 512
  mov r10, -1
  mov eax, SYS_epoll_wait
  syscall
  test rax, rax
  js .loop
  mov r12, rax
  xor edi, edi
  mov eax, SYS_time
  syscall
  mov [now_s], rax
  xor r13, r13
.evloop:
  cmp r13, r12
  jae .afterev
  lea rbx, [evbuf]
  lea rbx, [rbx + r13*4]
  lea rbx, [rbx + r13*8]
  mov r14d, dword [rbx+4]
  cmp r14d, dword [lfd]
  jne .notlfd
  mov edi, dword [conn_hi]
  call accept_conns
  mov eax, dword [nconns]
  cmp eax, dword [conn_hi]
  jl .evnext
  cmp dword [spawned], 0
  je .pause
  call listen_stop
  jmp .evnext
.pause:
  mov edi, dword [epfd]
  mov esi, EPOLL_CTL_DEL
  mov edx, dword [lfd]
  lea r10, [ev_tmp]
  mov eax, SYS_epoll_ctl
  syscall
  mov dword [spawn_pending], 1
  jmp .evnext
.notlfd:
  cmp r14d, dword [ready_fd]
  jne .conn
  mov edi, r14d
  lea rsi, [onebyte]
  mov edx, 1
  xor eax, eax              ; read
  syscall
  mov r15, rax
  mov edi, dword [ready_fd]
  mov eax, SYS_close
  syscall
  mov dword [ready_fd], -1
  cmp r15, 1
  jne .childfailed
  cmp dword [lfd], 0
  jl .evnext
  call listen_stop
  jmp .evnext
.childfailed:
  mov dword [spawned], 0
  movsxd rdi, dword [lfd]
  test rdi, rdi
  js .evnext
  mov esi, EPOLLIN
  call ep_add
  jmp .evnext
.conn:
  mov eax, r14d
  shl eax, 5
  lea r10, [conns+rax]
  cmp byte [r10+C_OPEN], 0
  je .evnext
  mov eax, dword [rbx]
  test eax, EPOLLOUT
  jz .rdcheck
  movsxd rdi, r14d
  call on_writable
  jmp .evnext
.rdcheck:
  mov eax, dword [rbx]
  test eax, EPOLLIN | EPOLLHUP | EPOLLERR
  jz .evnext
  movsxd rdi, r14d
  call on_readable
.evnext:
  inc r13
  jmp .evloop
.afterev:
  call flush_batch
.defloop:
  cmp dword [ndef], 0
  je .tail
  mov r12d, dword [ndef]
  mov dword [ndef], 0
  xor r13, r13
.dl:
  cmp r13d, r12d
  jae .dflush
  lea rax, [deferred]
  mov r14d, dword [rax + r13*4]
  mov eax, r14d
  shl eax, 5
  lea r10, [conns+rax]
  cmp byte [r10+C_OPEN], 0
  je .dnext
  cmp dword [r10+C_INLEN], 0
  je .dnext
  cmp qword [r10+C_OUT], 0
  jne .dnext
  movsxd rdi, r14d
  mov rsi, [r10+C_IN]
  mov edx, dword [r10+C_INLEN]
  call process
.dnext:
  inc r13
  jmp .dl
.dflush:
  call flush_batch
  jmp .defloop
.tail:
  cmp dword [spawn_pending], 0
  je .nospawn
  call spawn_worker
.nospawn:
  cmp dword [lfd], 0
  jge .loop
  mov eax, dword [nconns]
  cmp eax, dword [conn_lo]
  jge .loop
  call listen_start
  jmp .loop

; main(edi=argc, rsi=argv, rdx=envp)
main:
  push rbx
  push r12
  push r13
  mov r12d, edi
  mov r13, rsi
  mov [envp_ptr], rdx
  mov dword [lfd], -1
  mov dword [ready_fd], -1
  mov dword [notify_fd], -1
  cmp r12d, 2               ; --shared <notify fd>
  jle .args_done
  mov rdi, [r13+16]
  call atoi_
  mov dword [notify_fd], eax
.args_done:
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
  ; SIGPIPE and SIGCHLD ignored (workers reaped automatically)
  mov qword [actbuf], 1
  mov qword [actbuf+8], 0
  mov qword [actbuf+16], 0
  mov qword [actbuf+24], 0
  mov edi, 13
  lea rsi, [actbuf]
  xor edx, edx
  mov r10, 8
  mov eax, SYS_rt_sigaction
  syscall
  mov edi, 17
  lea rsi, [actbuf]
  xor edx, edx
  mov r10, 8
  mov eax, SYS_rt_sigaction
  syscall
  ; RLIMIT_NOFILE soft -> hard
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
  mov rax, [rlbuf]          ; lim = min(cur, MAXFD)
  cmp rax, MAXFD
  jbe .limok
  mov rax, MAXFD
.limok:
  mov rcx, rax
  shr rcx, 4                ; lim/16
  cmp rcx, 64
  jge .h
  mov rcx, 64
.h:
  mov rdx, rax
  sub rdx, rcx
  mov dword [conn_hi], edx
  mov rcx, rdx
  shr rcx, 3
  sub rdx, rcx
  mov dword [conn_lo], edx
  sub rax, 16
  mov dword [conn_max], eax
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
  cmp r12d, 1               ; only the original process starts the checkpointer
  jne .run
  mov eax, 57               ; fork
  syscall
  test eax, eax
  jnz .run                  ; parent (or fork failed: the wal_autocheckpoint backstop remains)
  lea rax, [arg_ckpt]
  mov [child_argv+8], rax
  mov qword [child_argv+16], 0
  jmp exec_self
.run:
  call run
  xor eax, eax
  pop r13
  pop r12
  pop rbx
  ret
.missing:
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
