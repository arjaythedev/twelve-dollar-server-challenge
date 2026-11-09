; Routing, handlers and response framing. Each handler appends one complete HTTP response to the
; connection's output buffer (g_out_slot points at it).
%include "defs.inc"

extern buf_reserve, w_int, w_esc, memmove, memcpy, clock_gettime
extern jwt_verify, jwt_uid, jwt_uname, jwt_ulen, parse_positive, json_obj, json_unescape, find_byte
extern db_begin, db_feed, db_post, db_post_exists
extern td_insert_post, td_insert_like, td_health

global resp_begin, resp_end, resp_error, handle_request, g_out_slot, g_start_s
global E_BAD_REQUEST, E_BAD_REQUEST_len, E_INTERNAL, E_INTERNAL_len

section .rodata
LIT S200, `HTTP/1.1 200 OK\r\n`
LIT S201, `HTTP/1.1 201 Created\r\n`
LIT S400, `HTTP/1.1 400 Bad Request\r\n`
LIT S401, `HTTP/1.1 401 Unauthorized\r\n`
LIT S404, `HTTP/1.1 404 Not Found\r\n`
LIT S500, `HTTP/1.1 500 Internal Server Error\r\n`
LIT S503, `HTTP/1.1 503 Service Unavailable\r\n`
LIT HDRS, `Content-Type: application/json\r\nContent-Length: 0000\r\n\r\n`
LIT CRLF2, `\r\n\r\n`
LIT L_ERR_OPEN, '{"error":"'
LIT E_NOT_FOUND, 'not found'
LIT E_BAD_ID, 'invalid post id'
LIT E_NO_POST, 'post not found'
LIT E_MISSING, 'missing bearer token'
LIT E_INVALID, 'invalid or expired token'
LIT E_PAYLOAD, 'invalid token payload'
LIT E_MALFORMED, 'malformed JSON body'
LIT E_REQUIRED, 'body is required'
LIT E_TOO_LONG, 'body must be at most 500 characters'
LIT E_INTERNAL, 'internal server error'
LIT E_BAD_REQUEST, 'bad request'
LIT L_HEALTH_OK, '{"status":"ok","db":"ok","uptime_s":'
LIT L_HEALTH_BAD, '{"status":"degraded","db":"unreachable","error":"'
LIT E_NO_TX, 'cannot begin transaction'
LIT L_NEW_POST, '{"post":{"id":'
LIT L_BODY, ',"body":"'
LIT L_CA, '","created_at":"'
LIT L_AU, '","author":"'
LIT L_LC0, '","like_count":0}}'
LIT L_LIKED_NEW, '{"liked":true,"already_liked":false,"post_id":'
LIT L_LIKED_OLD, '{"liked":true,"already_liked":true,"post_id":'
K_BODY: db "body"
align 8
KEYS_BODY: dq K_BODY, 4

section .bss
alignb 16
g_out_slot: resq 1               ; &conn->out of the request being answered
g_rbuf:     resq 1               ; the buffer the response is written into
g_bstart:   resq 1               ; start of its body
g_start_s:  resq 1               ; monotonic seconds at startup
ts:         resq 2
slot:       resb J_SIZE
newid:      resq 1
clen:       resq 1
created:    resb 64
errbuf:     resb 320
g_ubody:    resb MAX_BODY + 64

section .text

; ---- resp_begin(edi = status, rsi = body bound) -> rax = where the body goes, or 0 ---------------
resp_begin:
  push rbx
  push r12
  sub rsp, 8
  mov ebx, edi
  mov rdi, [g_out_slot]
  add rsi, 192
  call buf_reserve
  test rax, rax
  jz .r
  mov [g_rbuf], rax
  mov ecx, [rax + B_LEN]
  lea rdi, [rax + B_DATA + rcx]
  cmp ebx, 200
  jne .n201
  WLIT S200
  jmp .h
.n201:
  cmp ebx, 201
  jne .n400
  WLIT S201
  jmp .h
.n400:
  cmp ebx, 400
  jne .n401
  WLIT S400
  jmp .h
.n401:
  cmp ebx, 401
  jne .n404
  WLIT S401
  jmp .h
.n404:
  cmp ebx, 404
  jne .n503
  WLIT S404
  jmp .h
.n503:
  cmp ebx, 503
  jne .n500
  WLIT S503
  jmp .h
.n500:
  WLIT S500
.h:
  WLIT HDRS
  mov [g_bstart], rdi
  mov rax, rdi
.r:
  add rsp, 8
  pop r12
  pop rbx
  ret

; ---- resp_end(rdi = end of body): fills in Content-Length and the buffer's length --------------
resp_end:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, [g_bstart]
  mov r13, rdi
  sub r13, r12                   ; body length
  ; its digits, backwards into the red zone below the saved registers
  sub rsp, 32
  mov rax, r13
  lea r14, [rsp + 24]
  mov r9, 0xcccccccccccccccd
.d:
  mov rcx, rax
  mul r9
  shr rdx, 3
  lea rax, [rdx + rdx * 4]
  add rax, rax
  sub rcx, rax
  add cl, '0'
  dec r14
  mov [r14], cl
  mov rax, rdx
  test rax, rax
  jnz .d
  lea r15, [rsp + 24]
  sub r15, r14                   ; digit count
  lea rbx, [r12 - 8]             ; the "0000" placeholder
  cmp r15, 4
  je .copy
  ; move the body so the header has exactly the digits it needs
  lea rdi, [r12 + r15 - 4]
  mov rsi, r12
  mov rdx, r13
  call memmove
  lea rax, [rbx + r15]
  mov edx, [CRLF2]
  mov [rax], edx
  lea r12, [r12 + r15 - 4]
.copy:
  xor ecx, ecx
.c:
  mov al, [r14 + rcx]
  mov [rbx + rcx], al
  inc rcx
  cmp rcx, r15
  jb .c
  add rsp, 32
  mov rax, [g_rbuf]
  lea rcx, [r12 + r13]
  sub rcx, rax
  sub rcx, B_DATA
  mov [rax + B_LEN], ecx
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ---- resp_error(edi = status, rsi = message, rdx = length) -------------------------------------
resp_error:
  push rbx
  push r12
  sub rsp, 8
  mov rbx, rsi
  mov r12, rdx
  lea rsi, [rdx + 16]
  call resp_begin
  test rax, rax
  jz .r
  mov rdi, rax
  WLIT L_ERR_OPEN
  push rdi
  mov rsi, rbx
  mov rdx, r12
  sub rsp, 8
  call memcpy
  add rsp, 8
  pop rdi
  add rdi, r12
  mov word [rdi], '"}'
  add rdi, 2
  call resp_end
.r:
  add rsp, 8
  pop r12
  pop rbx
  ret

%macro ERR 2   ; status, message label
  mov edi, %1
  lea rsi, [%2]
  mov edx, %2_len
  call resp_error
%endmacro

; ---- handle_request(rdi = parsed request) -------------------------------------------------------
handle_request:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12, [rbx + Q_PATH]
  mov r13, [rbx + Q_PLEN]
  mov rax, [rbx + Q_METHOD]
  test rax, rax
  jz .get
  cmp rax, 1
  je .post
  jmp .nf
.get:
  cmp r13, 5
  jne .g1
  cmp dword [r12], '/fee'
  jne .nf
  cmp byte [r12 + 4], 'd'
  jne .nf
  call h_feed
  jmp .r
.g1:
  cmp r13, 7
  jb .nf
  cmp dword [r12], '/pos'
  jne .g2
  cmp word [r12 + 4], 'ts'
  jne .g2
  cmp byte [r12 + 6], '/'
  jne .g2
  lea rdi, [r12 + 7]
  lea rsi, [r12 + r13]
  mov edx, '/'
  call find_byte
  test rax, rax
  jnz .nf
  lea rdi, [r12 + 7]
  lea rsi, [r13 - 7]
  call h_get_post
  jmp .r
.g2:
  cmp r13, 7
  jne .nf
  cmp dword [r12], '/hea'
  jne .nf
  cmp word [r12 + 4], 'lt'
  jne .nf
  cmp byte [r12 + 6], 'h'
  jne .nf
  call h_health
  jmp .r
.post:
  cmp r13, 6
  jne .p1
  cmp dword [r12], '/pos'
  jne .nf
  cmp word [r12 + 4], 'ts'
  jne .nf
  mov rdi, rbx
  call h_create
  jmp .r
.p1:
  cmp r13, 12
  jb .nf
  cmp dword [r12], '/pos'
  jne .nf
  cmp word [r12 + 4], 'ts'
  jne .nf
  cmp byte [r12 + 6], '/'
  jne .nf
  cmp dword [r12 + r13 - 5], '/lik'
  jne .nf
  cmp byte [r12 + r13 - 1], 'e'
  jne .nf
  lea rdi, [r12 + 7]
  lea rsi, [r12 + r13 - 5]
  mov edx, '/'
  call find_byte
  test rax, rax
  jnz .nf
  mov rdi, rbx
  lea rsi, [r12 + 7]
  lea rdx, [r13 - 12]
  call h_like
  jmp .r
.nf:
  ERR 404, E_NOT_FOUND
.r:
  pop r13
  pop r12
  pop rbx
  ret

; ---- handlers -----------------------------------------------------------------------------------

h_feed:
  sub rsp, 8
  call db_feed
  test eax, eax
  jz .r
  ERR 500, E_INTERNAL
.r:
  add rsp, 8
  ret

; h_get_post(rdi = id text, rsi = length)
h_get_post:
  push rbx
  call parse_positive
  test rax, rax
  jz .bad
  mov rbx, rax
  call db_begin
  test eax, eax
  jnz .err
  mov rdi, rbx
  call db_post
  test eax, eax
  jz .r
  js .err
  ERR 404, E_NO_POST
  jmp .r
.bad:
  ERR 400, E_BAD_ID
  jmp .r
.err:
  ERR 500, E_INTERNAL
.r:
  pop rbx
  ret

h_health:
  push rbx
  call db_begin
  test eax, eax
  jnz .notx
  lea rdi, [errbuf]
  mov esi, 256
  call td_health
  test eax, eax
  jnz .down
  mov edi, 1                     ; CLOCK_MONOTONIC
  lea rsi, [ts]
  call clock_gettime
  mov edi, 200
  mov esi, 64
  call resp_begin
  test rax, rax
  jz .r
  mov rdi, rax
  WLIT L_HEALTH_OK
  mov rsi, [ts]
  sub rsi, [g_start_s]
  call w_int
  mov byte [rax], '}'
  lea rdi, [rax + 1]
  call resp_end
  jmp .r
.notx:
  lea rsi, [E_NO_TX]
  mov ebx, E_NO_TX_len
  jmp .bad
.down:
  lea rdi, [errbuf]
  xor eax, eax
.len:
  cmp byte [rdi + rax], 0
  je .lend
  inc eax
  jmp .len
.lend:
  mov ebx, eax
  lea rsi, [errbuf]
.bad:
  push rsi
  sub rsp, 8
  mov edi, 503
  imul esi, ebx, 6
  add esi, 64
  call resp_begin
  add rsp, 8
  pop rsi
  test rax, rax
  jz .r
  mov rdi, rax
  WLIT L_HEALTH_BAD
  mov edx, ebx
  call w_esc
  mov word [rax], '"}'
  lea rdi, [rax + 2]
  call resp_end
.r:
  pop rbx
  ret

; auth(rdi = request) -> eax 0 ok; otherwise the 401 is written and eax != 0
auth:
  sub rsp, 8
  mov rsi, [rdi + Q_ALEN]
  mov rdi, [rdi + Q_AUTH]
  call jwt_verify
  test eax, eax
  jz .r
  cmp eax, 1
  jne .a2
  ERR 401, E_MISSING
  jmp .f
.a2:
  cmp eax, 2
  jne .a3
  ERR 401, E_INVALID
  jmp .f
.a3:
  ERR 401, E_PAYLOAD
.f:
  mov eax, 1
.r:
  add rsp, 8
  ret

; h_create(rdi = request): POST /posts
h_create:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  call auth
  test eax, eax
  jnz .r
  mov rdi, [rbx + Q_BODY]
  mov rsi, [rbx + Q_BLEN]
  lea rdx, [KEYS_BODY]
  lea rcx, [slot]
  mov r8d, 1
  call json_obj
  test eax, eax
  js .malformed
  jz .required
  lea rax, [slot]
  cmp qword [rax + J_TYPE], 1
  jne .required
  mov r12, [rax + J_START]
  mov r13, [rax + J_LEN]
  ; decoded (or copied) into g_ubody, which has the slack w_esc reads into
  lea rdi, [g_ubody]
  mov rsi, r12
  mov rdx, r13
  call json_unescape
  lea r12, [g_ubody]
  mov r13, rax
  sub r13, r12
.trim:
  mov rdi, r12
  mov rsi, r13
  call js_trim
  mov r12, rax
  mov r13, rdx
  test r13, r13
  jz .required
  cmp r13, 500
  jbe .ok
  ; count code points: every byte that isn't a UTF-8 continuation byte
  xor ecx, ecx
  xor edx, edx
.cp:
  movzx eax, byte [r12 + rcx]
  and al, 0xc0
  cmp al, 0x80
  je .cont
  inc rdx
.cont:
  inc rcx
  cmp rcx, r13
  jb .cp
  cmp rdx, 500
  ja .too_long
.ok:
  call db_begin
  test eax, eax
  jnz .err
  mov rdi, [jwt_uid]
  mov rsi, r12
  mov edx, r13d
  lea rcx, [newid]
  lea r8, [created]
  lea r9, [clen]
  mov qword [clen], 0
  call td_insert_post
  test eax, eax
  jnz .err
  ; {"post":{"id":..,"body":..,"created_at":..,"author":..,"like_count":0}}
  mov rsi, r13
  add rsi, [clen]
  add rsi, [jwt_ulen]
  imul rsi, rsi, 6
  add rsi, 160
  mov edi, 201
  call resp_begin
  test rax, rax
  jz .r
  mov rdi, rax
  WLIT L_NEW_POST
  mov rsi, [newid]
  call w_int
  mov rdi, rax
  WLIT L_BODY
  mov rsi, r12
  mov rdx, r13
  call w_esc
  mov rdi, rax
  WLIT L_CA
  lea rsi, [created]
  mov edx, [clen]
  call w_esc
  mov rdi, rax
  WLIT L_AU
  lea rsi, [jwt_uname]
  mov rdx, [jwt_ulen]
  call w_esc
  mov rdi, rax
  WLIT L_LC0
  call resp_end
  jmp .r
.malformed:
  ERR 400, E_MALFORMED
  jmp .r
.required:
  ERR 400, E_REQUIRED
  jmp .r
.too_long:
  ERR 400, E_TOO_LONG
  jmp .r
.err:
  ERR 500, E_INTERNAL
.r:
  pop r13
  pop r12
  pop rbx
  ret

; h_like(rdi = request, rsi = id text, rdx = length): POST /posts/:id/like (auth first, then the id)
h_like:
  push rbx
  push r12
  push r13
  mov r12, rsi
  mov r13, rdx
  call auth
  test eax, eax
  jnz .r
  mov rdi, r12
  mov rsi, r13
  call parse_positive
  test rax, rax
  jz .bad
  mov rbx, rax
  call db_begin
  test eax, eax
  jnz .err
  mov rdi, rbx
  call db_post_exists
  test eax, eax
  js .err
  jz .missing
  mov rdi, [jwt_uid]
  mov rsi, rbx
  call td_insert_like
  test eax, eax
  js .err
  mov r12d, eax                  ; 1 new, 0 repeat
  mov edi, 201
  mov eax, 200
  test r12d, r12d
  cmovz edi, eax
  mov esi, 80
  call resp_begin
  test rax, rax
  jz .r
  mov rdi, rax
  test r12d, r12d
  jz .old
  WLIT L_LIKED_NEW
  jmp .id
.old:
  WLIT L_LIKED_OLD
.id:
  mov rsi, rbx
  call w_int
  mov byte [rax], '}'
  lea rdi, [rax + 1]
  call resp_end
  jmp .r
.bad:
  ERR 400, E_BAD_ID
  jmp .r
.missing:
  ERR 404, E_NO_POST
  jmp .r
.err:
  ERR 500, E_INTERNAL
.r:
  pop r13
  pop r12
  pop rbx
  ret

; ---- js_trim(rdi = p, rsi = len) -> rax = p, rdx = len -------------------------------------------
; JavaScript's String.prototype.trim: ASCII whitespace plus U+00A0, U+1680, U+2000-U+200A,
; U+2028, U+2029, U+202F, U+205F, U+3000 and U+FEFF, on valid UTF-8.
js_trim:
  lea rsi, [rdi + rsi]           ; end
.lead:
  cmp rdi, rsi
  jae .done
  movzx eax, byte [rdi]
  call ascii_ws
  je .l1
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 2
  jb .done
  cmp word [rdi], 0xa0c2
  je .l2
  cmp rcx, 3
  jb .done
  mov r8, rdi
  call ws3
  jne .done
  add rdi, 3
  jmp .lead
.l2:
  add rdi, 2
  jmp .lead
.l1:
  inc rdi
  jmp .lead
.done:
.trail:
  cmp rsi, rdi
  jbe .out
  movzx eax, byte [rsi - 1]
  call ascii_ws
  je .t1
  mov rcx, rsi
  sub rcx, rdi
  cmp rcx, 2
  jb .out
  cmp word [rsi - 2], 0xa0c2
  je .t2
  cmp rcx, 3
  jb .out
  lea r8, [rsi - 3]
  call ws3
  jne .out
  sub rsi, 3
  jmp .trail
.t2:
  sub rsi, 2
  jmp .trail
.t1:
  dec rsi
  jmp .trail
.out:
  mov rax, rdi
  mov rdx, rsi
  sub rdx, rdi
  ret

; ZF set if al is \t \n \v \f \r or space
ascii_ws:
  cmp al, ' '
  je .y
  sub al, 9
  cmp al, 4
  ja .n
  cmp al, al                     ; ZF = 1
.y:
  ret
.n:
  test esp, esp                  ; ZF = 0 (rsp is never 0)
  ret

; ZF set if the 3 bytes at r8 are one of the 3-byte whitespace characters
ws3:
  movzx eax, byte [r8]
  movzx ecx, byte [r8 + 1]
  movzx edx, byte [r8 + 2]
  cmp al, 0xe2
  je .e2
  cmp al, 0xe1
  je .e1
  cmp al, 0xe3
  je .e3
  cmp al, 0xef
  jne .no
  cmp cl, 0xbb
  jne .no
  cmp dl, 0xbf
  ret
.e1:
  cmp cl, 0x9a
  jne .no
  cmp dl, 0x80
  ret
.e3:
  cmp cl, 0x80
  jne .no
  cmp dl, 0x80
  ret
.e2:
  cmp cl, 0x81
  je .e281
  cmp cl, 0x80
  jne .no
  cmp dl, 0xa8
  je .yes
  cmp dl, 0xa9
  je .yes
  cmp dl, 0xaf
  je .yes
  sub dl, 0x80
  cmp dl, 0x0a
  ja .no
.yes:
  cmp al, al
  ret
.e281:
  cmp dl, 0x9f
  ret
.no:
  test esp, esp
  ret
