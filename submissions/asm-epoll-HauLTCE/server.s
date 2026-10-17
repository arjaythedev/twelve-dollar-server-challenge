# Twelve Dollar Server Challenge - x86_64 Linux Assembly Server

#  Architecture:
#   - 100% x86_64 assembly
#   - Singlethreaded non-blocking epoll event loop
#   - Direct HTTP/1.1 serving on HOST:PORT (0.0.0.0:80 or 127.0.0.1:3000)
#   - Register-level HTTP route dispatch & Base64url decoding
#   - OpenSSL libcrypto EVP_sha256 HMAC verification on every authenticated request
#   - SQLite3 WAL + synchronous=NORMAL + mmap_size=1GB + prepared statements
#   - Group Commit batching all writes per epoll tick into a single transaction

.intel_syntax noprefix
.global main

.extern getenv
.extern atoi
.extern inet_addr
.extern strlen
.extern strcpy
.extern memcpy
.extern snprintf
.extern time
.extern signal
.extern socket
.extern setsockopt
.extern bind
.extern listen
.extern accept4
.extern read
.extern write
.extern close
.extern epoll_create1
.extern epoll_ctl
.extern epoll_wait
.extern usleep

.extern sqlite3_open_v2
.extern sqlite3_exec
.extern sqlite3_prepare_v3
.extern sqlite3_reset
.extern sqlite3_clear_bindings
.extern sqlite3_bind_int64
.extern sqlite3_bind_text
.extern sqlite3_step
.extern sqlite3_column_int64
.extern sqlite3_column_text
.extern sqlite3_column_bytes
.extern sqlite3_changes

.extern EVP_sha256
.extern HMAC
.extern CRYPTO_memcmp

# CONSTANTS
.equ AF_INET,                2
.equ SOCK_STREAM_NONBLOCK,   0x80801     # SOCK_STREAM(1) | SOCK_NONBLOCK(0x800) | SOCK_CLOEXEC(0x80000)
.equ SOCK_NONBLOCK_CLOEXEC,  0x80800
.equ SOL_SOCKET,             1
.equ SO_REUSEADDR,           2
.equ SO_REUSEPORT,           15
.equ IPPROTO_TCP,            6
.equ TCP_NODELAY,            1
.equ EPOLL_CTL_ADD,          1
.equ EPOLL_CTL_DEL,          2
.equ EPOLLIN,                1
.equ MAX_EVENTS,             512
.equ MAX_BATCH,              512
.equ SQLITE_OPEN_RW_NOMUTEX, 0x00008002  # SQLITE_OPEN_READWRITE(2) | SQLITE_OPEN_NOMUTEX(0x8000)
.equ SQLITE_PREPARE_PERSIST, 1
.equ SQLITE_ROW,             100
.equ SQLITE_DONE,            101
.equ SQLITE_STATIC,          0

.equ OP_CREATE_POST,         1
.equ OP_LIKE_POST,           2

#   batch queue item layout (2304 bytes per item):
#       offset 0:    fd          (qword, 8 bytes)
#       offset 8:    op_type     (qword, 8 bytes)
#       offset 16:   uid         (qword, 8 bytes)
#       offset 24:   post_id     (qword, 8 bytes)
#       offset 32:   username    (128 bytes)
#       offset 160:  body        (2144 bytes, UTF-8 max 500 chars + null)
.equ BATCH_ITEM_SIZE,        2304

.section .rodata
.align 8

env_sqlite_path:    .asciz "SQLITE_PATH"
def_sqlite_path:    .asciz "seed/feed.db"
env_jwt_secret:     .asciz "JWT_SECRET"
def_jwt_secret:     .asciz "twelve-dollar-challenge"
env_host:           .asciz "HOST"
def_host:           .asciz "0.0.0.0"
env_port:           .asciz "PORT"
def_port:           .asciz "80"

sql_pragmas:
    .ascii "PRAGMA journal_mode = WAL;"
    .ascii "PRAGMA synchronous = NORMAL;"
    .ascii "PRAGMA temp_store = MEMORY;"
    .ascii "PRAGMA mmap_size = 1073741824;"
    .ascii "PRAGMA cache_size = -2000;"
    .ascii "PRAGMA wal_autocheckpoint = 0;"
    .asciz "PRAGMA busy_timeout = 5000;"

sql_health:
    .asciz "SELECT json_object('status','ok','db','ok','uptime_s',?1)"

sql_feed:
    .ascii "SELECT '{\"posts\":[' || COALESCE(group_concat(obj, ','), '') || ']}' "
    .ascii "FROM ("
    .ascii "  SELECT json_object("
    .ascii "    'id', p.id,"
    .ascii "    'body', p.body,"
    .ascii "    'created_at', p.created_at,"
    .ascii "    'author', (SELECT username FROM users u WHERE u.id = p.user_id),"
    .ascii "    'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
    .ascii "  ) AS obj "
    .ascii "  FROM posts p "
    .ascii "  ORDER BY p.created_at DESC, p.id DESC "
    .ascii "  LIMIT 20"
    .asciz ")"

sql_get_post:
    .ascii "SELECT json_object("
    .ascii "  'post', json_object("
    .ascii "    'id', p.id,"
    .ascii "    'body', p.body,"
    .ascii "    'created_at', p.created_at,"
    .ascii "    'author', (SELECT username FROM users u WHERE u.id = p.user_id),"
    .ascii "    'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
    .ascii "  )"
    .ascii ") "
    .asciz "FROM posts p WHERE p.id = ?1"

sql_jwt:
    .ascii "SELECT "
    .ascii "  CASE "
    .ascii "    WHEN NOT json_valid(?1) OR NOT json_valid(?2) THEN -1 "
    .ascii "    WHEN json_type(?1) != 'object' OR json_type(?2) != 'object' THEN -1 "
    .ascii "    WHEN json_extract(?1, '$.alg') IS NOT 'HS256' THEN -1 "
    .ascii "    WHEN json_type(?2, '$.exp') NOT IN ('integer', 'real') OR json_extract(?2, '$.exp') <= ?3 THEN -1 "
    .ascii "    WHEN json_type(?2, '$.username') IS NOT 'text' OR length(json_extract(?2, '$.username')) = 0 THEN -2 "
    .ascii "    WHEN json_type(?2, '$.sub') = 'integer' AND json_extract(?2, '$.sub') > 0 THEN json_extract(?2, '$.sub') "
    .ascii "    WHEN json_type(?2, '$.sub') = 'text' AND json_extract(?2, '$.sub') GLOB '[1-9]*' AND NOT json_extract(?2, '$.sub') GLOB '*[^0-9]*' THEN CAST(json_extract(?2, '$.sub') AS INTEGER) "
    .ascii "    ELSE -2 "
    .ascii "  END, "
    .ascii "  CASE "
    .ascii "    WHEN json_valid(?2) AND json_type(?2) = 'object' AND json_type(?2, '$.username') = 'text' THEN json_extract(?2, '$.username') "
    .ascii "    ELSE '' "
    .asciz "  END"

sql_validate_body:
    .ascii "SELECT "
    .ascii "  CASE "
    .ascii "    WHEN NOT json_valid(?1) THEN -1 "
    .ascii "    WHEN json_type(?1) != 'object' THEN -1 "
    .ascii "    WHEN json_type(?1, '$.body') IS NULL OR json_type(?1, '$.body') != 'text' THEN -2 "
    .ascii "    ELSE length(trim(json_extract(?1, '$.body'), ' ' || char(9, 10, 13))) "
    .ascii "  END, "
    .ascii "  CASE "
    .ascii "    WHEN json_valid(?1) AND json_type(?1) = 'object' AND json_type(?1, '$.body') = 'text' "
    .ascii "    THEN trim(json_extract(?1, '$.body'), ' ' || char(9, 10, 13)) "
    .ascii "    ELSE '' "
    .asciz "  END"

sql_insert_post:
    .ascii "INSERT INTO posts (user_id, body) VALUES (?1, ?2) "
    .ascii "RETURNING json_object("
    .ascii "  'post', json_object("
    .ascii "    'id', id,"
    .ascii "    'body', body,"
    .ascii "    'created_at', created_at,"
    .ascii "    'author', ?3,"
    .ascii "    'like_count', 0"
    .ascii "  )"
    .asciz ")"

sql_post_exists:
    .asciz "SELECT 1 FROM posts WHERE id = ?1"

sql_insert_like:
    .asciz "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING"

sql_begin:
    .asciz "BEGIN IMMEDIATE"

sql_commit:
    .asciz "COMMIT"

sql_rollback:
    .asciz "ROLLBACK"

err_not_found:      .asciz "{\"error\":\"not found\"}"
err_invalid_id:     .asciz "{\"error\":\"invalid post id\"}"
err_post_not_found: .asciz "{\"error\":\"post not found\"}"
err_missing_token:  .asciz "{\"error\":\"missing bearer token\"}"
err_invalid_token:  .asciz "{\"error\":\"invalid or expired token\"}"
err_invalid_pay:    .asciz "{\"error\":\"invalid token payload\"}"
err_malformed_json: .asciz "{\"error\":\"malformed JSON body\"}"
err_body_required:  .asciz "{\"error\":\"body is required\"}"
err_body_too_long:  .asciz "{\"error\":\"body must be at most 500 characters\"}"
err_internal:       .asciz "{\"error\":\"internal server error\"}"
err_degraded:       .asciz "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"db failed\"}"

fmt_like_new:       .asciz "{\"liked\":true,\"already_liked\":false,\"post_id\":%lld}"
fmt_like_repeat:    .asciz "{\"liked\":true,\"already_liked\":true,\"post_id\":%lld}"

fmt_http_resp:
    .ascii "HTTP/1.1 %d %s\r\n"
    .ascii "Content-Type: application/json\r\n"
    .ascii "Connection: keep-alive\r\n"
    .asciz "Content-Length: %zu\r\n\r\n"

status_200:         .asciz "OK"
status_201:         .asciz "Created"
status_400:         .asciz "Bad Request"
status_401:         .asciz "Unauthorized"
status_404:         .asciz "Not Found"
status_500:         .asciz "Internal Server Error"
status_503:         .asciz "Service Unavailable"

str_get:            .asciz "GET"
str_post:           .asciz "POST"
str_health:         .asciz "/health"
str_feed:           .asciz "/feed"
str_posts:          .asciz "/posts"
str_posts_slash:    .asciz "/posts/"
str_slash_like:     .asciz "/like"
str_auth_prefix:    .asciz "authorization:"
str_cl_prefix:      .asciz "content-length:"
str_bearer_prefix:  .asciz "Bearer "

# 256-byte base64url decoding lookup table (0 -> 63 valid, 0xFF invalid)
.align 64
b64url_table:
    .rept 43
    .byte 0xFF
    .endr
    .byte 62                                                                             # 43: +
    .byte 0xFF                                                                           # 44: ,
    .byte 62                                                                             # 45: -
    .byte 0xFF                                                                           # 46: .
    .byte 63                                                                             # 47: /
    .byte 52,53,54,55,56,57,58,59,60,61                                                  # 48 -> 57: 0 -> 9
    .rept 7
    .byte 0xFF
    .endr
    .byte 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25            # 65 -> 90: A -> Z
    .byte 0xFF,0xFF,0xFF,0xFF
    .byte 63                                                                             # 95: _
    .byte 0xFF                                                                           # 96: `
    .byte 26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51 # 97 -> 122: a -> z
    .rept 133
    .byte 0xFF
    .endr

.section .bss
.align 64

g_start_time:       .quad 0
g_db:               .quad 0
g_jwt_secret:       .quad 0
g_jwt_secret_len:   .quad 0
g_listen_fd:        .quad 0
g_epoll_fd:         .quad 0

#prepared sqlite statement pointers
g_stmt_health:      .quad 0
g_stmt_feed:        .quad 0
g_stmt_get_post:    .quad 0
g_stmt_jwt:         .quad 0
g_stmt_val_body:    .quad 0
g_stmt_ins_post:    .quad 0
g_stmt_post_exists: .quad 0
g_stmt_ins_like:    .quad 0
g_stmt_begin:       .quad 0
g_stmt_commit:      .quad 0
g_stmt_rollback:    .quad 0

#authenticated user output for current request
g_auth_uid:         .quad 0
g_auth_username:    .space 256

#scratch buffers
g_sockaddr:         .space 16
g_epoll_ev:         .space 16
g_epoll_events:     .space (MAX_EVENTS * 12)
g_req_buf:          .space 32768
g_resp_buf:         .space 65536
g_like_json_buf:    .space 256
g_jwt_hdr_buf:      .space 2048
g_jwt_pay_buf:      .space 4096
g_jwt_sig_buf:      .space 256
g_hmac_out:         .space 64
g_hmac_len:         .long 0

#group commit queue
g_batch_count:      .quad 0
g_batch_ticks:      .quad 0
g_batch_queue:      .space (MAX_BATCH * BATCH_ITEM_SIZE)

.section .text

#helper: send_http_response(edi = fd, esi = status_code, rdx = body_ptr, rcx = body_len)
send_http_response:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8

    mov r12d, edi               #fd
    mov r13d, esi               #status_code
    mov r14, rdx                #body_ptr
    mov r15, rcx                #body_len

    lea r8, [rip + status_200]
    cmp r13d, 200
    je .L_status_ready
    lea r8, [rip + status_201]
    cmp r13d, 201
    je .L_status_ready
    lea r8, [rip + status_400]
    cmp r13d, 400
    je .L_status_ready
    lea r8, [rip + status_401]
    cmp r13d, 401
    je .L_status_ready
    lea r8, [rip + status_404]
    cmp r13d, 404
    je .L_status_ready
    lea r8, [rip + status_503]
    cmp r13d, 503
    je .L_status_ready
    lea r8, [rip + status_500]

.L_status_ready:
    lea rdi, [rip + g_resp_buf]
    mov rsi, 1024
    lea rdx, [rip + fmt_http_resp]
    mov ecx, r13d
    mov r9, r15
    xor eax, eax
    call snprintf
    mov rbx, rax                # header length

    test r15, r15
    jz .L_write_out
    lea rdi, [rip + g_resp_buf]
    add rdi, rbx
    mov rsi, r14
    mov rdx, r15
    call memcpy
    add rbx, r15

.L_write_out:
    lea r14, [rip + g_resp_buf]
.L_write_loop:
    test rbx, rbx
    jle .L_send_done
    mov edi, r12d
    mov rsi, r14
    mov rdx, rbx
    call write
    test rax, rax
    jle .L_send_done
    add r14, rax
    sub rbx, rax
    jmp .L_write_loop

.L_send_done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

# helper: send_cstr_response(edi = fd, esi = status_code, rdx = cstr_ptr)
send_cstr_response:
    push rbp
    mov rbp, rsp
    push r12
    push r13
    push r14
    sub rsp, 8

    mov r12d, edi
    mov r13d, esi
    mov r14, rdx

    mov rdi, r14
    call strlen

    mov edi, r12d
    mov esi, r13d
    mov rdx, r14
    mov rcx, rax
    call send_http_response

    add rsp, 8
    pop r14
    pop r13
    pop r12
    pop rbp
    ret

# helper: parse_positive_int(rdi = str_ptr, rsi = str_len) -> rax = int64 (>0) or -1
parse_positive_int:
    test rsi, rsi
    jle .L_ppi_bad
    cmp rsi, 18
    jg .L_ppi_bad

    movzx ecx, byte ptr [rdi]
    cmp cl, '1'
    jl .L_ppi_bad
    cmp cl, '9'
    jg .L_ppi_bad

    xor eax, eax
    xor r8d, r8d
.L_ppi_loop:
    cmp r8, rsi
    jge .L_ppi_done
    movzx ecx, byte ptr [rdi + r8]
    cmp cl, '0'
    jl .L_ppi_bad
    cmp cl, '9'
    jg .L_ppi_bad
    sub ecx, '0'
    imul rax, rax, 10
    add rax, rcx
    inc r8
    jmp .L_ppi_loop

.L_ppi_done:
    test rax, rax
    jle .L_ppi_bad
    ret

.L_ppi_bad:
    mov rax, -1
    ret

#  helper: base64url_decode(rdi = src_ptr, rsi = src_len, rdx = dst_ptr) -> rax = dec_len or -1
base64url_decode:
    test rsi, rsi
    jle .L_b64_bad
.L_b64_strip:
    test rsi, rsi
    jz .L_b64_bad
    movzx ecx, byte ptr [rdi + rsi - 1]
    cmp cl, '='
    jne .L_b64_check_rem
    dec rsi
    jmp .L_b64_strip

.L_b64_check_rem:
    mov rax, rsi
    and rax, 3
    cmp rax, 1
    je .L_b64_bad               #len % 4 == 1 => invalid base64

    lea r8, [rip + b64url_table]
    xor r9d, r9d                #src index
    xor r10d, r10d              #dst index
    xor ecx, ecx                #bit accumulator
    xor r11d, r11d              #bits collected

.L_b64_loop:
    cmp r9, rsi
    jge .L_b64_finish
    movzx eax, byte ptr [rdi + r9]
    movzx eax, byte ptr [r8 + rax]
    cmp al, 0xFF
    je .L_b64_bad
    shl ecx, 6
    or ecx, eax
    add r11d, 6
    cmp r11d, 8
    jl .L_b64_next
    sub r11d, 8
    mov eax, ecx
    push rcx
    mov ecx, r11d
    shr eax, cl
    pop rcx
    mov byte ptr [rdx + r10], al
    inc r10
.L_b64_next:
    inc r9
    jmp .L_b64_loop

.L_b64_finish:
    mov rax, r10
    ret

.L_b64_bad:
    mov rax, -1
    ret


#helper: find_bearer_token(rdi = req_ptr, rsi = headers_len)
# return: rax = token_ptr (or 0 if missing/not Bearer), rdx = token_len
find_bearer_token:
    push rbx
    push r12
    push r13

    mov r12, rdi                #req_ptr
    lea r13, [rdi + rsi]        #headers_end

    mov rbx, r12
.L_fbt_skip_reqline:
    cmp rbx, r13
    jge .L_fbt_missing
    cmp byte ptr [rbx], 10      #\n
    je .L_fbt_line_start
    inc rbx
    jmp .L_fbt_skip_reqline

.L_fbt_line_start:
    inc rbx
    mov rax, r13
    sub rax, rbx
    cmp rax, 21                 #>= Authorization: Bearer
    jl .L_fbt_missing

    lea r8, [rip + str_auth_prefix]
    xor ecx, ecx
.L_fbt_chk_auth:
    cmp ecx, 14
    je .L_fbt_matched_auth
    movzx eax, byte ptr [rbx + rcx]
    cmp al, 'A'
    jl .L_fbt_no_lower
    cmp al, 'Z'
    jg .L_fbt_no_lower
    or al, 0x20
.L_fbt_no_lower:
    cmp al, byte ptr [r8 + rcx]
    jne .L_fbt_next_line
    inc ecx
    jmp .L_fbt_chk_auth

.L_fbt_next_line:
    cmp rbx, r13
    jge .L_fbt_missing
    cmp byte ptr [rbx], 10      #\n
    je .L_fbt_line_start
    inc rbx
    jmp .L_fbt_next_line

.L_fbt_matched_auth:
    add rbx, 14
.L_fbt_skip_ws:
    cmp rbx, r13
    jge .L_fbt_missing
    movzx eax, byte ptr [rbx]
    cmp al, ' '
    je .L_fbt_inc_ws
    cmp al, 9                   #\t
    je .L_fbt_inc_ws
    jmp .L_fbt_check_bearer
.L_fbt_inc_ws:
    inc rbx
    jmp .L_fbt_skip_ws

.L_fbt_check_bearer:
    mov rax, r13
    sub rax, rbx
    cmp rax, 7
    jl .L_fbt_missing
    lea r8, [rip + str_bearer_prefix]
    xor ecx, ecx
.L_fbt_chk_b_loop:
    cmp ecx, 7
    je .L_fbt_got_bearer
    movzx eax, byte ptr [rbx + rcx]
    cmp al, byte ptr [r8 + rcx]
    jne .L_fbt_missing
    inc ecx
    jmp .L_fbt_chk_b_loop

.L_fbt_got_bearer:
    add rbx, 7
    mov rax, rbx
.L_fbt_scan_end:
    cmp rbx, r13
    jge .L_fbt_tok_done
    movzx ecx, byte ptr [rbx]
    cmp cl, 13                  #\r
    je .L_fbt_tok_done
    cmp cl, 10                  #\n
    je .L_fbt_tok_done
    cmp cl, ' '
    je .L_fbt_tok_done
    inc rbx
    jmp .L_fbt_scan_end

.L_fbt_tok_done:
    mov rdx, rbx
    sub rdx, rax
    test rdx, rdx
    jle .L_fbt_missing
    pop r13
    pop r12
    pop rbx
    ret

.L_fbt_missing:
    xor eax, eax
    xor edx, edx
    pop r13
    pop r12
    pop rbx
    ret

# helper: verify_jwt(rdi = token_ptr, rsi = token_len)
# return: rax = uid (>0) on success, -1 on invalid/expired, -2 on invalid payload
verify_jwt:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24

    mov r12, rdi                #token_ptr
    mov r13, rsi                #token_len

    mov r14, -1                 #dot1
    mov r15, -1                 #dot2
    xor ebx, ebx
.L_jwt_scan_dots:
    cmp rbx, r13
    jge .L_jwt_dots_done
    cmp byte ptr [r12 + rbx], '.'
    jne .L_jwt_dot_next
    cmp r14, -1
    jne .L_jwt_second_dot
    mov r14, rbx
    jmp .L_jwt_dot_next
.L_jwt_second_dot:
    cmp r15, -1
    jne .L_jwt_err_invalid      #more than 2 dots
    mov r15, rbx
.L_jwt_dot_next:
    inc rbx
    jmp .L_jwt_scan_dots

.L_jwt_dots_done:
    cmp r14, 0
    jle .L_jwt_err_invalid
    cmp r15, r14
    jle .L_jwt_err_invalid
    mov rax, r15
    sub rax, r14
    cmp rax, 1
    jle .L_jwt_err_invalid      # empty payload

    mov rbx, r13
    sub rbx, r15
    dec rbx
    test rbx, rbx
    jle .L_jwt_err_invalid
    cmp rbx, 128
    jg .L_jwt_err_invalid
    cmp r14, 1024
    jg .L_jwt_err_invalid
    mov rax, r15
    sub rax, r14
    cmp rax, 2048
    jg .L_jwt_err_invalid

    lea rdi, [r12 + r15 + 1]
    mov rsi, rbx
    lea rdx, [rip + g_jwt_sig_buf]
    call base64url_decode
    cmp rax, 32
    jne .L_jwt_err_invalid

    call EVP_sha256
    mov rdi, rax
    mov rsi, qword ptr [rip + g_jwt_secret]
    mov edx, dword ptr [rip + g_jwt_secret_len]
    mov rcx, r12                #data = token_ptr
    mov r8, r15                 #data_len = dot2
    lea r9, [rip + g_hmac_out]
    lea rax, [rip + g_hmac_len]
    mov qword ptr [rsp], rax    #7th arg on stack
    call HMAC
    test rax, rax
    jz .L_jwt_err_invalid

    lea rdi, [rip + g_hmac_out]
    lea rsi, [rip + g_jwt_sig_buf]
    mov edx, 32
    call CRYPTO_memcmp
    test eax, eax
    jnz .L_jwt_err_invalid

    mov rdi, r12
    mov rsi, r14
    lea rdx, [rip + g_jwt_hdr_buf]
    call base64url_decode
    test rax, rax
    jle .L_jwt_err_invalid
    lea rcx, [rip + g_jwt_hdr_buf]
    mov byte ptr [rcx + rax], 0

    lea rdi, [r12 + r14 + 1]
    mov rsi, r15
    sub rsi, r14
    dec rsi
    lea rdx, [rip + g_jwt_pay_buf]
    call base64url_decode
    test rax, rax
    jle .L_jwt_err_invalid
    lea rcx, [rip + g_jwt_pay_buf]
    mov byte ptr [rcx + rax], 0

    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_reset

    mov rdi, qword ptr [rip + g_stmt_jwt]
    mov esi, 1
    lea rdx, [rip + g_jwt_hdr_buf]
    mov ecx, -1
    xor r8d, r8d                #SQLITE_STATIC
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_jwt]
    mov esi, 2
    lea rdx, [rip + g_jwt_pay_buf]
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    xor edi, edi
    call time
    mov rdx, rax
    mov rdi, qword ptr [rip + g_stmt_jwt]
    mov esi, 3
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_jwt_reset_and_invalid

    mov rdi, qword ptr [rip + g_stmt_jwt]
    xor esi, esi
    call sqlite3_column_int64
    mov rbx, rax
    test rbx, rbx
    jle .L_jwt_reset_and_ret_rbx #-1 or -2

    mov qword ptr [rip + g_auth_uid], rbx

    mov rdi, qword ptr [rip + g_stmt_jwt]
    mov esi, 1
    call sqlite3_column_text
    test rax, rax
    jz .L_jwt_reset_and_payload
    lea rdi, [rip + g_auth_username]
    mov rsi, rax
    call strcpy

    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_reset
    mov rax, rbx
    jmp .L_jwt_done

.L_jwt_reset_and_ret_rbx:
    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_reset
    mov rax, rbx
    jmp .L_jwt_done

.L_jwt_reset_and_payload:
    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_reset
    mov rax, -2
    jmp .L_jwt_done

.L_jwt_reset_and_invalid:
    mov rdi, qword ptr [rip + g_stmt_jwt]
    call sqlite3_reset

.L_jwt_err_invalid:
    mov rax, -1

.L_jwt_done:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

# helper: flush_group_commit()
flush_group_commit:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8

    mov r12, qword ptr [rip + g_batch_count]
    test r12, r12
    jz .L_fgc_exit

    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_reset
    cmp r14d, SQLITE_DONE
    jne .L_fgc_abort_batch

    xor ebx, ebx
    lea r13, [rip + g_batch_queue]

.L_fgc_exec_loop:
    cmp rbx, r12
    jge .L_fgc_commit

    mov rax, qword ptr [r13 + 8]    #op_type
    cmp rax, OP_CREATE_POST
    je .L_fgc_do_create
    jmp .L_fgc_do_like

.L_fgc_do_create:
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 1
    mov rdx, qword ptr [r13 + 16]   #uid
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 2
    lea rdx, [r13 + 160]            #trimmed body
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 3
    lea rdx, [r13 + 32]             #username
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_fgc_create_err

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    xor esi, esi
    call sqlite3_column_text
    mov r14, rax

    mov qword ptr [r13 + 24], 201
    lea rdi, [r13 + 160]
    mov rsi, r14
    call strcpy

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset
    jmp .L_fgc_next_item

.L_fgc_create_err:
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset
    mov qword ptr [r13 + 24], 500
    lea rdi, [r13 + 160]
    lea rsi, [rip + err_internal]
    call strcpy
    jmp .L_fgc_next_item

.L_fgc_do_like:
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    mov esi, 1
    mov rdx, qword ptr [r13 + 24]   #post_id
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_reset
    cmp r14d, SQLITE_ROW
    jne .L_fgc_like_404

    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    mov esi, 1
    mov rdx, qword ptr [r13 + 16]   #uid
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    mov esi, 2
    mov rdx, qword ptr [r13 + 24]   #post_id
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_step
    mov r15d, eax
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_reset
    cmp r15d, SQLITE_DONE
    jne .L_fgc_create_err

    mov rdi, qword ptr [rip + g_db]
    call sqlite3_changes
    mov r14, qword ptr [r13 + 24]   #post_id
    test eax, eax
    jg .L_fgc_like_201

    mov qword ptr [r13 + 24], 200
    lea rdi, [r13 + 160]
    mov rsi, 256
    lea rdx, [rip + fmt_like_repeat]
    mov rcx, r14
    xor eax, eax
    call snprintf
    jmp .L_fgc_next_item

.L_fgc_like_201:
    mov qword ptr [r13 + 24], 201
    lea rdi, [r13 + 160]
    mov rsi, 256
    lea rdx, [rip + fmt_like_new]
    mov rcx, r14
    xor eax, eax
    call snprintf
    jmp .L_fgc_next_item

.L_fgc_like_404:
    mov qword ptr [r13 + 24], 404
    lea rdi, [r13 + 160]
    lea rsi, [rip + err_post_not_found]
    call strcpy

.L_fgc_next_item:
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .L_fgc_exec_loop

.L_fgc_commit:
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    cmp r14d, SQLITE_DONE
    jne .L_fgc_rollback_and_500

    xor ebx, ebx
    lea r13, [rip + g_batch_queue]
.L_fgc_send_loop:
    cmp rbx, r12
    jge .L_fgc_reset
    mov edi, dword ptr [r13]        #fd
    mov esi, dword ptr [r13 + 24]   #http status code
    lea rdx, [r13 + 160]            #json body cstr
    call send_cstr_response
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .L_fgc_send_loop

.L_fgc_rollback_and_500:
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_reset

.L_fgc_abort_batch:
    xor ebx, ebx
    lea r13, [rip + g_batch_queue]
.L_fgc_err_loop:
    cmp rbx, r12
    jge .L_fgc_reset
    mov edi, dword ptr [r13]
    mov esi, 500
    lea rdx, [rip + err_internal]
    call send_cstr_response
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .L_fgc_err_loop

.L_fgc_reset:
    mov qword ptr [rip + g_batch_count], 0
    mov qword ptr [rip + g_batch_ticks], 0

.L_fgc_exit:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

#gelper: handle_client_request(edi = fd, rsi = req_ptr, rdx = total_len)
handle_client_request:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24

    mov r12d, edi               #fd
    mov r13, rsi                #req_ptr
    mov r14, rdx                #total_len

    xor ebx, ebx
    mov rcx, r14
    sub rcx, 3
.L_hcr_find_hdr_end:
    cmp rbx, rcx
    jge .L_hcr_404
    cmp dword ptr [r13 + rbx], 0x0a0d0a0d
    je .L_hcr_found_hdr_end
    inc rbx
    jmp .L_hcr_find_hdr_end

.L_hcr_found_hdr_end:
    add rbx, 4
    mov qword ptr [rsp], rbx

    cmp dword ptr [r13], 0x20544547     #GET 
    je .L_hcr_is_get
    cmp dword ptr [r13], 0x54534f50     #POST
    jne .L_hcr_404
    cmp byte ptr [r13 + 4], ' '
    jne .L_hcr_404

    lea r15, [r13 + 5]
    xor ecx, ecx
.L_hcr_post_path_len:
    movzx eax, byte ptr [r15 + rcx]
    cmp al, ' '
    je .L_hcr_post_dispatch
    cmp al, '?'
    je .L_hcr_post_dispatch
    cmp al, 13
    je .L_hcr_post_dispatch
    test al, al
    jz .L_hcr_404
    inc rcx
    jmp .L_hcr_post_path_len

.L_hcr_post_dispatch:
    mov qword ptr [rsp + 8], rcx    #path_len
    cmp rcx, 6
    jne .L_hcr_check_post_like
    cmp dword ptr [r15], 0x736f702f     #/pos
    jne .L_hcr_404
    cmp word ptr [r15 + 4], 0x7374      #ts
    jne .L_hcr_404

    mov rdi, r13
    mov rsi, qword ptr [rsp]        #headers_len
    call find_bearer_token
    test rax, rax
    jz .L_hcr_401_missing

    mov rdi, rax
    mov rsi, rdx
    call verify_jwt
    cmp rax, -1
    je .L_hcr_401_invalid
    cmp rax, -2
    je .L_hcr_401_payload

    mov rbx, qword ptr [rsp]        #headers_len
    lea rdi, [r13 + rbx]            #body_ptr
    mov rsi, r14
    sub rsi, rbx                    #body_len
    mov byte ptr [rdi + rsi], 0

    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_val_body]
    mov esi, 1
    mov rbx, qword ptr [rsp]
    lea rdx, [r13 + rbx]
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_hcr_val_reset_malformed

    mov rdi, qword ptr [rip + g_stmt_val_body]
    xor esi, esi
    call sqlite3_column_int64
    mov r14, rax
    cmp r14, -1
    je .L_hcr_val_reset_malformed
    cmp r14, -2
    je .L_hcr_val_reset_required
    test r14, r14
    jle .L_hcr_val_reset_required
    cmp r14, 500
    jg .L_hcr_val_reset_too_long

    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12                #fd
    mov qword ptr [r15 + 8], OP_CREATE_POST
    mov rax, qword ptr [rip + g_auth_uid]
    mov qword ptr [r15 + 16], rax           #uid

    lea rdi, [r15 + 32]
    lea rsi, [rip + g_auth_username]
    call strcpy

    mov rdi, qword ptr [rip + g_stmt_val_body]
    mov esi, 1
    call sqlite3_column_text
    lea rdi, [r15 + 160]
    mov rsi, rax
    call strcpy

    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset

    inc qword ptr [rip + g_batch_count]
    jmp .L_hcr_done

.L_hcr_val_reset_malformed:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .L_hcr_400_malformed

.L_hcr_val_reset_required:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .L_hcr_400_required

.L_hcr_val_reset_too_long:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .L_hcr_400_too_long

.L_hcr_check_post_like:
    cmp rcx, 12
    jle .L_hcr_404
    cmp dword ptr [r15], 0x736f702f         #/pos
    jne .L_hcr_404
    cmp word ptr [r15 + 4], 0x7374          #ts
    jne .L_hcr_404
    cmp byte ptr [r15 + 6], '/'
    jne .L_hcr_404

    lea rax, [r15 + rcx - 5]
    cmp dword ptr [rax], 0x6b696c2f         #/lik
    jne .L_hcr_404
    cmp byte ptr [rax + 4], 'e'
    jne .L_hcr_404

    lea rdi, [r15 + 7]
    mov rsi, rcx
    sub rsi, 12                             #id_len = path_len - 12
    xor edx, edx
.L_hcr_chk_slash_like:
    cmp rdx, rsi
    jge .L_hcr_like_auth
    cmp byte ptr [rdi + rdx], '/'
    je .L_hcr_404
    inc rdx
    jmp .L_hcr_chk_slash_like

.L_hcr_like_auth:
    mov rdi, r13
    mov rsi, qword ptr [rsp]                #headers_len
    call find_bearer_token
    test rax, rax
    jz .L_hcr_401_missing

    mov rdi, rax
    mov rsi, rdx
    call verify_jwt
    cmp rax, -1
    je .L_hcr_401_invalid
    cmp rax, -2
    je .L_hcr_401_payload

    lea rdi, [r15 + 7]
    mov rsi, qword ptr [rsp + 8]
    sub rsi, 12
    call parse_positive_int
    test rax, rax
    jle .L_hcr_400_invalid_id

    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12                #fd
    mov qword ptr [r15 + 8], OP_LIKE_POST
    mov rcx, qword ptr [rip + g_auth_uid]
    mov qword ptr [r15 + 16], rcx           #uid
    mov qword ptr [r15 + 24], rax           #post_id

    inc qword ptr [rip + g_batch_count]
    jmp .L_hcr_done

    # GET routes
.L_hcr_is_get:
    lea r15, [r13 + 4]                      #path_ptr
    xor ecx, ecx
.L_hcr_get_path_len:
    movzx eax, byte ptr [r15 + rcx]
    cmp al, ' '
    je .L_hcr_get_dispatch
    cmp al, '?'
    je .L_hcr_get_dispatch
    cmp al, 13
    je .L_hcr_get_dispatch
    test al, al
    jz .L_hcr_404
    inc rcx
    jmp .L_hcr_get_path_len

.L_hcr_get_dispatch:
    cmp rcx, 5
    jne .L_hcr_chk_health
    cmp dword ptr [r15], 0x6565662f         #/fee
    jne .L_hcr_404
    cmp byte ptr [r15 + 4], 'd'
    jne .L_hcr_404

    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_hcr_feed_err

    mov rdi, qword ptr [rip + g_stmt_feed]
    xor esi, esi
    call sqlite3_column_text
    mov rbx, rax

    mov rdi, qword ptr [rip + g_stmt_feed]
    xor esi, esi
    call sqlite3_column_bytes
    mov rcx, rax

    mov edi, r12d
    mov esi, 200
    mov rdx, rbx
    call send_http_response

    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_reset
    jmp .L_hcr_done

.L_hcr_feed_err:
    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_reset
    jmp .L_hcr_500

.L_hcr_chk_health:
    cmp rcx, 7
    jne .L_hcr_chk_get_post
    cmp dword ptr [r15], 0x6165682f         #/hea
    jne .L_hcr_404
    cmp word ptr [r15 + 4], 0x746c          #lt
    jne .L_hcr_404
    cmp byte ptr [r15 + 6], 'h'
    jne .L_hcr_404

    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_reset

    xor edi, edi
    call time
    sub rax, qword ptr [rip + g_start_time]
    mov rdx, rax
    mov rdi, qword ptr [rip + g_stmt_health]
    mov esi, 1
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_hcr_health_err

    mov rdi, qword ptr [rip + g_stmt_health]
    xor esi, esi
    call sqlite3_column_text
    mov rbx, rax

    mov rdi, qword ptr [rip + g_stmt_health]
    xor esi, esi
    call sqlite3_column_bytes
    mov rcx, rax

    mov edi, r12d
    mov esi, 200
    mov rdx, rbx
    call send_http_response

    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_reset
    jmp .L_hcr_done

.L_hcr_health_err:
    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_reset
    jmp .L_hcr_503

.L_hcr_chk_get_post:
    cmp rcx, 7
    jle .L_hcr_404
    cmp dword ptr [r15], 0x736f702f         #/pos
    jne .L_hcr_404
    cmp word ptr [r15 + 4], 0x7374          #ts
    jne .L_hcr_404
    cmp byte ptr [r15 + 6], '/'
    jne .L_hcr_404

    lea rdi, [r15 + 7]
    mov rsi, rcx
    sub rsi, 7
    xor edx, edx
.L_hcr_chk_post_slash:
    cmp rdx, rsi
    jge .L_hcr_parse_get_id
    cmp byte ptr [rdi + rdx], '/'
    je .L_hcr_404
    inc rdx
    jmp .L_hcr_chk_post_slash

.L_hcr_parse_get_id:
    call parse_positive_int
    test rax, rax
    jle .L_hcr_400_invalid_id

    mov rbx, rax                            #post_id
    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 1
    mov rdx, rbx
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .L_hcr_get_post_not_found

    mov rdi, qword ptr [rip + g_stmt_get_post]
    xor esi, esi
    call sqlite3_column_text
    mov rbx, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    xor esi, esi
    call sqlite3_column_bytes
    mov rcx, rax

    mov edi, r12d
    mov esi, 200
    mov rdx, rbx
    call send_http_response

    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset
    jmp .L_hcr_done

.L_hcr_get_post_not_found:
    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset
    jmp .L_hcr_404_post

    #errors
.L_hcr_404:
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_not_found]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_404_post:
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_post_not_found]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_400_invalid_id:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_invalid_id]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_400_malformed:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_malformed_json]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_400_required:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_required]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_400_too_long:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_too_long]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_401_missing:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_missing_token]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_401_invalid:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_token]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_401_payload:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_pay]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_503:
    mov edi, r12d
    mov esi, 503
    lea rdx, [rip + err_degraded]
    call send_cstr_response
    jmp .L_hcr_done

.L_hcr_500:
    mov edi, r12d
    mov esi, 500
    lea rdx, [rip + err_internal]
    call send_cstr_response

.L_hcr_done:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret


# MAIN
main:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24

    mov edi, 13
    mov esi, 1
    call signal

    xor edi, edi
    call time
    mov qword ptr [rip + g_start_time], rax

    lea rdi, [rip + env_jwt_secret]
    call getenv
    test rax, rax
    jnz .L_got_jwt_sec
    lea rax, [rip + def_jwt_secret]
.L_got_jwt_sec:
    mov qword ptr [rip + g_jwt_secret], rax
    mov rdi, rax
    call strlen
    mov qword ptr [rip + g_jwt_secret_len], rax

    lea rdi, [rip + env_sqlite_path]
    call getenv
    test rax, rax
    jnz .L_got_db_path
    lea rax, [rip + def_sqlite_path]
.L_got_db_path:
    mov rdi, rax
    lea rsi, [rip + g_db]
    mov edx, SQLITE_OPEN_RW_NOMUTEX
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_pragmas]
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_health]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_health]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_feed]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_feed]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_get_post]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_get_post]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_jwt]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_jwt]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_validate_body]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_val_body]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_insert_post]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_ins_post]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_post_exists]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_post_exists]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_insert_like]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_ins_like]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_begin]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_begin]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_commit]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_commit]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_rollback]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    lea r8, [rip + g_stmt_rollback]
    xor r9d, r9d
    call sqlite3_prepare_v3
    test eax, eax
    jnz .L_fatal

    mov edi, AF_INET
    mov esi, SOCK_STREAM_NONBLOCK
    xor edx, edx
    call socket
    test eax, eax
    js .L_fatal
    mov qword ptr [rip + g_listen_fd], rax
    mov r12d, eax

    mov dword ptr [rsp], 1
    mov edi, r12d
    mov esi, SOL_SOCKET
    mov edx, SO_REUSEADDR
    mov rcx, rsp
    mov r8d, 4
    call setsockopt

    mov edi, r12d
    mov esi, SOL_SOCKET
    mov edx, SO_REUSEPORT
    mov rcx, rsp
    mov r8d, 4
    call setsockopt

    lea r13, [rip + g_sockaddr]
    mov word ptr [r13], AF_INET

    lea rdi, [rip + env_port]
    call getenv
    test rax, rax
    jnz .L_got_port
    lea rax, [rip + def_port]
.L_got_port:
    mov rdi, rax
    call atoi
    rol ax, 8                               #htons(port)
    mov word ptr [r13 + 2], ax

    lea rdi, [rip + env_host]
    call getenv
    test rax, rax
    jnz .L_got_host
    lea rax, [rip + def_host]
.L_got_host:
    mov rdi, rax
    call inet_addr
    mov dword ptr [r13 + 4], eax

    mov edi, r12d
    mov rsi, r13
    mov edx, 16
    call bind
    test eax, eax
    jnz .L_fatal

    mov edi, r12d
    mov esi, 65535
    call listen
    test eax, eax
    jnz .L_fatal

    xor edi, edi
    call epoll_create1
    test eax, eax
    js .L_fatal
    mov qword ptr [rip + g_epoll_fd], rax
    mov r13d, eax

    lea rcx, [rip + g_epoll_ev]
    mov dword ptr [rcx], EPOLLIN
    mov qword ptr [rcx + 4], r12
    mov edi, r13d
    mov esi, EPOLL_CTL_ADD
    mov edx, r12d
    call epoll_ctl



    # MAIN EVENT LOOP
.L_event_loop:
    mov ecx, -1
    cmp qword ptr [rip + g_batch_count], 0
    je .L_do_epoll_wait
    mov ecx, 2
.L_do_epoll_wait:
    mov edi, r13d
    lea rsi, [rip + g_epoll_events]
    mov edx, MAX_EVENTS
    call epoll_wait
    test eax, eax
    jg .L_got_events
    cmp qword ptr [rip + g_batch_count], 0
    je .L_event_loop
    call flush_group_commit
    jmp .L_event_loop
.L_got_events:

    mov r14d, eax                           #num_events
    xor r15d, r15d

.L_process_event:
    cmp r15d, r14d
    jge .L_end_tick

    imul ebx, r15d, 12                      #sizeof(struct epoll_event) == 12 on x86_64
    lea rcx, [rip + g_epoll_events]
    mov eax, dword ptr [rcx + rbx + 4]      #event.data.fd
    cmp eax, r12d
    je .L_accept_loop

    mov ebx, eax                            #client_fd
    mov edi, ebx
    lea rsi, [rip + g_req_buf]
    mov edx, 32000
    call read
    test rax, rax
    jle .L_close_client

    mov rdx, rax                            #bytes read
    lea rsi, [rip + g_req_buf]
    mov byte ptr [rsi + rdx], 0

    mov edi, ebx
    call handle_client_request

    cmp qword ptr [rip + g_batch_count], MAX_BATCH
    jl .L_next_event
    call flush_group_commit
    jmp .L_next_event

.L_close_client:
    mov edi, r13d
    mov esi, EPOLL_CTL_DEL
    mov edx, ebx
    xor ecx, ecx
    call epoll_ctl
    mov edi, ebx
    call close
    jmp .L_next_event

.L_accept_loop:
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov ecx, SOCK_NONBLOCK_CLOEXEC
    call accept4
    test eax, eax
    js .L_next_event

    mov ebx, eax                            #client_fd
    mov dword ptr [rsp], 1
    mov edi, ebx
    mov esi, IPPROTO_TCP
    mov edx, TCP_NODELAY
    mov rcx, rsp
    mov r8d, 4
    call setsockopt

    lea rcx, [rip + g_epoll_ev]
    mov dword ptr [rcx], EPOLLIN
    mov dword ptr [rcx + 4], ebx
    mov dword ptr [rcx + 8], 0
    mov edi, r13d
    mov esi, EPOLL_CTL_ADD
    mov edx, ebx
    call epoll_ctl
    jmp .L_accept_loop

.L_next_event:
    inc r15d
    jmp .L_process_event

.L_end_tick:
    cmp qword ptr [rip + g_batch_count], 0
    je .L_event_loop
    inc qword ptr [rip + g_batch_ticks]
    cmp qword ptr [rip + g_batch_count], 64
    jge .L_flush_now
    cmp qword ptr [rip + g_batch_ticks], 16
    jl .L_event_loop
.L_flush_now:
    call flush_group_commit
    jmp .L_event_loop

.L_fatal:
    mov eax, 1
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

.section .note.GNU-stack,"",@progbits















