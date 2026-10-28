# 12$ server challenge - x86_64 linux asm (gas, intel syntax)
# gcc -no-pie -O3 server.s -o bin/server -lsqlite3 -lcrypto

.intel_syntax noprefix
.global main

.extern getenv
.extern atoi
.extern inet_addr
.extern strlen
.extern strcpy
.extern memcpy
.extern time
.extern signal
.extern socket
.extern setsockopt
.extern bind
.extern listen

.extern sqlite3_open_v2
.extern sqlite3_exec
.extern sqlite3_prepare_v3
.extern sqlite3_reset
.extern sqlite3_bind_int64
.extern sqlite3_bind_text
.extern sqlite3_step
.extern sqlite3_column_int64
.extern sqlite3_column_text
.extern sqlite3_changes

.extern EVP_sha256
.extern HMAC
.extern CRYPTO_memcmp

.equ SYS_READ,               0
.equ SYS_WRITE,              1
.equ SYS_CLOSE,              3
.equ SYS_EPOLL_WAIT,         232
.equ SYS_EPOLL_CTL,          233
.equ SYS_ACCEPT4,            288
.equ SYS_EPOLL_CREATE1,      291

.equ AF_INET,                2
.equ SOCK_STREAM_NONBLOCK,   0x80801
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
.equ KEEP_ALIVE_FD_LIMIT,    65000
.equ SQLITE_OPEN_RW_NOMUTEX, 0x00008002
.equ SQLITE_PREPARE_PERSIST, 1
.equ SQLITE_ROW,             100
.equ SQLITE_DONE,            101

.equ OP_CREATE_POST,         1
.equ OP_LIKE_POST,           2
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

msg_up:             .ascii "[asm] listening\n"
.equ LEN_MSG_UP,    . - msg_up
msg_fatal:          .ascii "fatal: init failed\n"
.equ LEN_MSG_FATAL, . - msg_fatal

# warm posts index last so likes scan doesnt evict posts from the 16mb cache
sql_pragmas:
    .ascii "PRAGMA journal_mode = WAL;"
    .ascii "PRAGMA synchronous = NORMAL;"
    .ascii "PRAGMA temp_store = MEMORY;"
    .ascii "PRAGMA mmap_size = 1073741824;"
    .ascii "PRAGMA cache_size = -16000;"
    .ascii "PRAGMA wal_autocheckpoint = 0;"
    .ascii "PRAGMA journal_size_limit = 67108864;"
    .ascii "PRAGMA busy_timeout = 5000;"
    .ascii "SELECT count(*) FROM likes INDEXED BY likes_post_id_idx;"
    .ascii "SELECT count(*) FROM users;"
    .asciz "SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx;"

sql_health:
    .asciz "SELECT 1"

sql_feed:
    .ascii "SELECT p.id, p.body, p.created_at, "
    .ascii "(SELECT username FROM users u WHERE u.id = p.user_id), "
    .ascii "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    .asciz "FROM posts p ORDER BY p.created_at DESC, p.id DESC LIMIT 20"

sql_get_post:
    .ascii "SELECT p.id, p.body, p.created_at, "
    .ascii "(SELECT username FROM users u WHERE u.id = p.user_id), "
    .ascii "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    .asciz "FROM posts p WHERE p.id = ?1"

# lazy way out for utf8 char count + whitespace trim on POST /posts
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
    .asciz "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at"

sql_post_exists:
    .asciz "SELECT 1 FROM posts WHERE id = ?1"

sql_insert_like:
    .asciz "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING"

sql_begin_read:     .asciz "BEGIN"
sql_begin:          .asciz "BEGIN IMMEDIATE"
sql_commit:         .asciz "COMMIT"
sql_rollback:       .asciz "ROLLBACK"
sql_ckpt:           .asciz "PRAGMA wal_checkpoint(PASSIVE)"

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

str_health_prefix:  .ascii "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"
.equ LEN_HEALTH_PFX, . - str_health_prefix

str_feed_prefix:    .ascii "{\"posts\":["
.equ LEN_FEED_PFX,  . - str_feed_prefix

str_post_prefix:    .ascii "{\"post\":"
.equ LEN_POST_PFX,  . - str_post_prefix

str_p_body:         .ascii ",\"body\":\""
str_p_created:      .ascii "\",\"created_at\":\""
str_p_author:       .ascii "\",\"author\":\""
str_p_likes:        .ascii "\",\"like_count\":"

str_like_new_pfx:   .ascii "{\"liked\":true,\"already_liked\":false,\"post_id\":"
.equ LEN_LIKE_NEW,  . - str_like_new_pfx
str_like_rep_pfx:   .ascii "{\"liked\":true,\"already_liked\":true,\"post_id\":"
.equ LEN_LIKE_REP,  . - str_like_rep_pfx

hdr_200_ka:         .ascii "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_200_KA,    . - hdr_200_ka
hdr_201_ka:         .ascii "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_201_KA,    . - hdr_201_ka
hdr_400_ka:         .ascii "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_400_KA,    . - hdr_400_ka
hdr_401_ka:         .ascii "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_401_KA,    . - hdr_401_ka
hdr_404_ka:         .ascii "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_404_KA,    . - hdr_404_ka
hdr_503_ka:         .ascii "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_503_KA,    . - hdr_503_ka
hdr_500_ka:         .ascii "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: "
.equ LEN_500_KA,    . - hdr_500_ka

hdr_200_cl:         .ascii "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
.equ LEN_200_CL,    . - hdr_200_cl
hdr_201_cl:         .ascii "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
.equ LEN_201_CL,    . - hdr_201_cl

str_auth_prefix:    .asciz "authorization:"
str_bearer_prefix:  .asciz "Bearer "
str_alg_hs256:      .asciz "\"alg\":\"HS256\""
str_exp_key:        .asciz "\"exp\":"
str_sub_key:        .asciz "\"sub\":"
str_user_key:       .asciz "\"username\":\""

.align 64
b64url_table:
    .rept 43
    .byte 0xFF
    .endr
    .byte 62
    .byte 0xFF
    .byte 62
    .byte 0xFF
    .byte 63
    .byte 52,53,54,55,56,57,58,59,60,61
    .rept 7
    .byte 0xFF
    .endr
    .byte 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25
    .byte 0xFF,0xFF,0xFF,0xFF
    .byte 63
    .byte 0xFF
    .byte 26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51
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
g_in_tx:            .quad 0
g_ckpt_counter:     .quad 0

g_stmt_health:      .quad 0
g_stmt_feed:        .quad 0
g_stmt_get_post:    .quad 0
g_stmt_val_body:    .quad 0
g_stmt_ins_post:    .quad 0
g_stmt_post_exists: .quad 0
g_stmt_ins_like:    .quad 0
g_stmt_begin_read:  .quad 0
g_stmt_begin:       .quad 0
g_stmt_commit:      .quad 0
g_stmt_rollback:    .quad 0
g_stmt_ckpt:        .quad 0

g_auth_uid:         .quad 0
g_auth_username:    .space 256

g_sockaddr:         .space 16
g_epoll_ev:         .space 16
g_epoll_events:     .space (MAX_EVENTS * 12)
g_req_buf:          .space 32768
g_resp_buf:         .space 65536
g_json_buf:         .space 65536
g_num_tmp:          .space 32
g_jwt_hdr_buf:      .space 2048
g_jwt_pay_buf:      .space 4096
g_jwt_sig_buf:      .space 256
g_hmac_out:         .space 64
g_hmac_len:         .long 0

g_batch_count:      .quad 0
g_batch_queue:      .space (MAX_BATCH * BATCH_ITEM_SIZE)

.section .text

append_u64:
    mov rax, rsi
    lea r8, [rip + g_num_tmp + 24]
    mov rcx, r8
    mov r9, 10
1:
    xor edx, edx
    div r9
    add dl, '0'
    dec r8
    mov byte ptr [r8], dl
    test rax, rax
    jnz 1b
    sub rcx, r8
    xor eax, eax
2:
    cmp rax, rcx
    jge 3f
    movzx edx, byte ptr [r8 + rax]
    mov byte ptr [rdi + rax], dl
    inc rax
    jmp 2b
3:
    lea rax, [rdi + rcx]
    ret

append_cstr:
    xor ecx, ecx
1:
    movzx edx, byte ptr [rsi + rcx]
    test dl, dl
    jz 2f
    mov byte ptr [rdi + rcx], dl
    inc rcx
    jmp 1b
2:
    lea rax, [rdi + rcx]
    ret

append_json_escaped:
    mov rax, rdi
.esc_loop:
    movzx edx, byte ptr [rsi]
    test dl, dl
    jz .esc_done
    inc rsi
    cmp dl, '"'
    je .esc_slash
    cmp dl, '\\'
    je .esc_slash
    cmp dl, 10
    je .esc_n
    cmp dl, 13
    je .esc_r
    cmp dl, 9
    je .esc_t
    mov byte ptr [rax], dl
    inc rax
    jmp .esc_loop
.esc_slash:
    mov byte ptr [rax], '\\'
    mov byte ptr [rax + 1], dl
    add rax, 2
    jmp .esc_loop
.esc_n:
    mov word ptr [rax], 0x6e5c
    add rax, 2
    jmp .esc_loop
.esc_r:
    mov word ptr [rax], 0x725c
    add rax, 2
    jmp .esc_loop
.esc_t:
    mov word ptr [rax], 0x745c
    add rax, 2
    jmp .esc_loop
.esc_done:
    ret

# only calls our own leaf helpers above, so r10/r11 survive
append_post_json:
    push rbx
    push r12
    push r13

    mov rbx, rdi
    mov r12, rdx                    # body
    mov r13, rcx                    # created_at
    mov r10, r8                     # author
    mov r11, r9                     # like_count

    mov dword ptr [rbx], 0x6469227b # {"id":
    mov word ptr [rbx + 4], 0x3a22
    lea rdi, [rbx + 6]
    call append_u64
    mov rbx, rax

    mov rax, qword ptr [rip + str_p_body]
    mov qword ptr [rbx], rax
    mov byte ptr [rbx + 8], '"'
    lea rdi, [rbx + 9]
    mov rsi, r12
    call append_json_escaped
    mov rbx, rax

    mov rax, qword ptr [rip + str_p_created]
    mov rcx, qword ptr [rip + str_p_created + 8]
    mov qword ptr [rbx], rax
    mov qword ptr [rbx + 8], rcx
    lea rdi, [rbx + 16]
    mov rsi, r13
    call append_cstr
    mov rbx, rax

    mov rax, qword ptr [rip + str_p_author]
    mov ecx, dword ptr [rip + str_p_author + 8]
    mov qword ptr [rbx], rax
    mov dword ptr [rbx + 8], ecx
    lea rdi, [rbx + 12]
    mov rsi, r10
    call append_cstr
    mov rbx, rax

    mov rax, qword ptr [rip + str_p_likes]
    mov rcx, qword ptr [rip + str_p_likes + 7]
    mov qword ptr [rbx], rax
    mov qword ptr [rbx + 7], rcx
    lea rdi, [rbx + 15]
    mov rsi, r11
    call append_u64
    mov byte ptr [rax], '}'
    inc rax

    pop r13
    pop r12
    pop rbx
    ret

# no external calls here either, just rep movsb + syscalls
send_http_response:
    push rbx
    push r12

    mov r12d, edi                   # fd
    mov eax, esi                    # status
    mov r10, rdx                    # body_ptr
    mov r11, rcx                    # body_len

    # fuck it, anything past fd 65000 gets Connection: close so it never hit EMFILE
    cmp r12d, KEEP_ALIVE_FD_LIMIT
    jg .use_close_hdr

    lea rsi, [rip + hdr_200_ka]
    mov ecx, LEN_200_KA
    cmp eax, 200
    je .do_hdr_cp
    lea rsi, [rip + hdr_201_ka]
    mov ecx, LEN_201_KA
    cmp eax, 201
    je .do_hdr_cp
    lea rsi, [rip + hdr_400_ka]
    mov ecx, LEN_400_KA
    cmp eax, 400
    je .do_hdr_cp
    lea rsi, [rip + hdr_401_ka]
    mov ecx, LEN_401_KA
    cmp eax, 401
    je .do_hdr_cp
    lea rsi, [rip + hdr_404_ka]
    mov ecx, LEN_404_KA
    cmp eax, 404
    je .do_hdr_cp
    lea rsi, [rip + hdr_503_ka]
    mov ecx, LEN_503_KA
    cmp eax, 503
    je .do_hdr_cp
    lea rsi, [rip + hdr_500_ka]
    mov ecx, LEN_500_KA
    jmp .do_hdr_cp

.use_close_hdr:
    lea rsi, [rip + hdr_201_cl]
    mov ecx, LEN_201_CL
    cmp eax, 201
    je .do_hdr_cp
    lea rsi, [rip + hdr_200_cl]
    mov ecx, LEN_200_CL

.do_hdr_cp:
    lea rdi, [rip + g_resp_buf]
    rep movsb

    mov rsi, r11
    call append_u64
    mov dword ptr [rax], 0x0a0d0a0d
    add rax, 4
    lea rcx, [rip + g_resp_buf]
    mov rbx, rax
    sub rbx, rcx

    test r11, r11
    jz .flush_sock
    mov rdi, rax
    mov rsi, r10
    mov rcx, r11
    rep movsb
    add rbx, r11

.flush_sock:
    lea r10, [rip + g_resp_buf]
.write_again:
    test rbx, rbx
    jle .maybe_evict
    mov eax, SYS_WRITE
    mov edi, r12d
    mov rsi, r10
    mov rdx, rbx
    syscall
    test rax, rax
    jle .maybe_evict
    add r10, rax
    sub rbx, rax
    jmp .write_again

.maybe_evict:
    cmp r12d, KEEP_ALIVE_FD_LIMIT
    jle .resp_out
    mov eax, SYS_EPOLL_CTL
    mov edi, dword ptr [rip + g_epoll_fd]
    mov esi, EPOLL_CTL_DEL
    mov edx, r12d
    xor r10d, r10d
    syscall
    mov eax, SYS_CLOSE
    mov edi, r12d
    syscall

.resp_out:
    pop r12
    pop rbx
    ret

send_cstr_response:
    xor ecx, ecx
1:
    cmp byte ptr [rdx + rcx], 0
    je 2f
    inc rcx
    jmp 1b
2:
    jmp send_http_response

parse_positive_int:
    test rsi, rsi
    jle .bad_int
    cmp rsi, 18
    jg .bad_int

    movzx ecx, byte ptr [rdi]
    cmp cl, '1'
    jl .bad_int
    cmp cl, '9'
    jg .bad_int

    xor eax, eax
    xor r8d, r8d
.int_loop:
    cmp r8, rsi
    jge .int_ok
    movzx ecx, byte ptr [rdi + r8]
    cmp cl, '0'
    jl .bad_int
    cmp cl, '9'
    jg .bad_int
    sub ecx, '0'
    imul rax, rax, 10
    add rax, rcx
    inc r8
    jmp .int_loop

.int_ok:
    test rax, rax
    jle .bad_int
    ret

.bad_int:
    mov rax, -1
    ret

# keep bit count in cl and accum in r11d so it doesnt touch the stack in the loop
base64url_decode:
    test rsi, rsi
    jle .b64_err
1:
    test rsi, rsi
    jz .b64_err
    cmp byte ptr [rdi + rsi - 1], '='
    jne 2f
    dec rsi
    jmp 1b
2:
    mov rax, rsi
    and rax, 3
    cmp rax, 1
    je .b64_err

    lea r8, [rip + b64url_table]
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor ecx, ecx

.b64_char:
    cmp r9, rsi
    jge .b64_done
    movzx eax, byte ptr [rdi + r9]
    movzx eax, byte ptr [r8 + rax]
    cmp al, 0xFF
    je .b64_err
    shl r11d, 6
    or r11d, eax
    add ecx, 6
    cmp ecx, 8
    jl .b64_next
    sub ecx, 8
    mov eax, r11d
    shr eax, cl
    mov byte ptr [rdx + r10], al
    inc r10
.b64_next:
    inc r9
    jmp .b64_char

.b64_done:
    mov rax, r10
    ret

.b64_err:
    mov rax, -1
    ret

find_after_substr:
    mov r8, rdi
1:
    cmp byte ptr [r8], 0
    je .no_sub
    xor ecx, ecx
2:
    movzx edx, byte ptr [rsi + rcx]
    test dl, dl
    jz .got_sub
    movzx eax, byte ptr [r8 + rcx]
    cmp al, dl
    jne 3f
    inc rcx
    jmp 2b
3:
    inc r8
    jmp 1b
.got_sub:
    lea rax, [r8 + rcx]
    ret
.no_sub:
    xor eax, eax
    ret

find_bearer_token:
    lea r9, [rdi + rsi]
    mov r8, rdi
1:
    cmp r8, r9
    jge .no_tok
    cmp byte ptr [r8], 10
    je .hdr_line
    inc r8
    jmp 1b

.hdr_line:
    inc r8
    mov rax, r9
    sub rax, r8
    cmp rax, 21
    jl .no_tok

    lea r10, [rip + str_auth_prefix]
    xor ecx, ecx
.chk_auth:
    cmp ecx, 14
    je .got_auth_hdr
    movzx eax, byte ptr [r8 + rcx]
    cmp al, 'A'
    jl 2f
    cmp al, 'Z'
    jg 2f
    or al, 0x20
2:
    cmp al, byte ptr [r10 + rcx]
    jne .skip_line
    inc ecx
    jmp .chk_auth

.skip_line:
    cmp r8, r9
    jge .no_tok
    cmp byte ptr [r8], 10
    je .hdr_line
    inc r8
    jmp .skip_line

.got_auth_hdr:
    add r8, 14
.trim_ws:
    cmp r8, r9
    jge .no_tok
    movzx eax, byte ptr [r8]
    cmp al, ' '
    je 3f
    cmp al, 9
    je 3f
    jmp .chk_bearer
3:
    inc r8
    jmp .trim_ws

.chk_bearer:
    mov rax, r9
    sub rax, r8
    cmp rax, 7
    jl .no_tok
    cmp dword ptr [r8], 0x72616542  # "Bear"
    jne .no_tok
    cmp word ptr [r8 + 4], 0x7265   # "er"
    jne .no_tok
    cmp byte ptr [r8 + 6], ' '
    jne .no_tok

    add r8, 7
    mov rax, r8
.scan_tok_end:
    cmp r8, r9
    jge .tok_out
    movzx ecx, byte ptr [r8]
    cmp cl, 13
    je .tok_out
    cmp cl, 10
    je .tok_out
    cmp cl, ' '
    je .tok_out
    inc r8
    jmp .scan_tok_end

.tok_out:
    mov rdx, r8
    sub rdx, rax
    test rdx, rdx
    jle .no_tok
    ret

.no_tok:
    xor eax, eax
    xor edx, edx
    ret

verify_jwt:
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16

    mov r12, rdi
    mov r13, rsi

    mov r14, -1                     # dot1
    mov r15, -1                     # dot2
    xor ebx, ebx
.dot_loop:
    cmp rbx, r13
    jge .dots_checked
    cmp byte ptr [r12 + rbx], '.'
    jne .dot_next
    cmp r14, -1
    jne .second_dot
    mov r14, rbx
    jmp .dot_next
.second_dot:
    cmp r15, -1
    jne .jwt_bad
    mov r15, rbx
.dot_next:
    inc rbx
    jmp .dot_loop

.dots_checked:
    cmp r14, 0
    jle .jwt_bad
    cmp r15, r14
    jle .jwt_bad
    mov rax, r15
    sub rax, r14
    cmp rax, 1
    jle .jwt_bad

    mov rbx, r13
    sub rbx, r15
    dec rbx
    test rbx, rbx
    jle .jwt_bad
    cmp rbx, 128
    jg .jwt_bad
    cmp r14, 1024
    jg .jwt_bad
    mov rax, r15
    sub rax, r14
    cmp rax, 2048
    jg .jwt_bad

    lea rdi, [r12 + r15 + 1]
    mov rsi, rbx
    lea rdx, [rip + g_jwt_sig_buf]
    call base64url_decode
    cmp rax, 32
    jne .jwt_bad

    call EVP_sha256
    mov rdi, rax
    mov rsi, qword ptr [rip + g_jwt_secret]
    mov edx, dword ptr [rip + g_jwt_secret_len]
    mov rcx, r12
    mov r8, r15
    lea r9, [rip + g_hmac_out]
    lea rax, [rip + g_hmac_len]
    mov qword ptr [rsp], rax
    call HMAC
    test rax, rax
    jz .jwt_bad

    lea rdi, [rip + g_hmac_out]
    lea rsi, [rip + g_jwt_sig_buf]
    mov edx, 32
    call CRYPTO_memcmp
    test eax, eax
    jnz .jwt_bad

    mov rdi, r12
    mov rsi, r14
    lea rdx, [rip + g_jwt_hdr_buf]
    call base64url_decode
    test rax, rax
    jle .jwt_bad
    lea rcx, [rip + g_jwt_hdr_buf]
    mov byte ptr [rcx + rax], 0

    lea rdi, [r12 + r14 + 1]
    mov rsi, r15
    sub rsi, r14
    dec rsi
    lea rdx, [rip + g_jwt_pay_buf]
    call base64url_decode
    test rax, rax
    jle .jwt_bad
    lea rcx, [rip + g_jwt_pay_buf]
    mov byte ptr [rcx + rax], 0

    lea rdi, [rip + g_jwt_hdr_buf]
    lea rsi, [rip + str_alg_hs256]
    call find_after_substr
    test rax, rax
    jz .jwt_bad

    lea rdi, [rip + g_jwt_pay_buf]
    lea rsi, [rip + str_exp_key]
    call find_after_substr
    test rax, rax
    jz .jwt_bad
    mov rdi, rax
    xor esi, esi
1:
    movzx ecx, byte ptr [rdi + rsi]
    cmp cl, '0'
    jl 2f
    cmp cl, '9'
    jg 2f
    inc rsi
    jmp 1b
2:
    call parse_positive_int
    test rax, rax
    jle .jwt_bad
    mov rbx, rax
    xor edi, edi
    call time
    cmp rbx, rax
    jle .jwt_bad

    # test.sh sends "sub":1 as int, seed/tokens.json sends "sub":"1" as string. handle both or suffer
    lea rdi, [rip + g_jwt_pay_buf]
    lea rsi, [rip + str_sub_key]
    call find_after_substr
    test rax, rax
    jz .jwt_bad_pay
    mov rdi, rax
    xor r9d, r9d
    cmp byte ptr [rdi], '"'
    jne 3f
    inc rdi
    mov r9d, 1
3:
    xor esi, esi
4:
    movzx ecx, byte ptr [rdi + rsi]
    cmp cl, '0'
    jl 5f
    cmp cl, '9'
    jg 5f
    inc rsi
    jmp 4b
5:
    test r9d, r9d
    jz .unquoted_sub
    cmp cl, '"'
    jne .jwt_bad_pay
    jmp .parse_sub
.unquoted_sub:
    cmp cl, ','
    je .parse_sub
    cmp cl, '}'
    je .parse_sub
    jmp .jwt_bad_pay
.parse_sub:
    call parse_positive_int
    test rax, rax
    jle .jwt_bad_pay
    mov rbx, rax
    mov qword ptr [rip + g_auth_uid], rbx

    lea rdi, [rip + g_jwt_pay_buf]
    lea rsi, [rip + str_user_key]
    call find_after_substr
    test rax, rax
    jz .jwt_bad_pay
    lea rdi, [rip + g_auth_username]
    xor ecx, ecx
6:
    cmp ecx, 250
    jge .jwt_bad_pay
    movzx edx, byte ptr [rax + rcx]
    test dl, dl
    jz .jwt_bad_pay
    cmp dl, '"'
    je 7f
    mov byte ptr [rdi + rcx], dl
    inc rcx
    jmp 6b
7:
    test ecx, ecx
    jz .jwt_bad_pay
    mov byte ptr [rdi + rcx], 0
    mov rax, rbx
    jmp .jwt_out

.jwt_bad_pay:
    mov rax, -2
    jmp .jwt_out

.jwt_bad:
    mov rax, -1

.jwt_out:
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

flush_group_commit:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, qword ptr [rip + g_batch_count]
    test r12, r12
    jz .fgc_done

    cmp qword ptr [rip + g_in_tx], 0
    jne .tx_open

    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_reset
    cmp r14d, SQLITE_DONE
    jne .abort_batch
    mov qword ptr [rip + g_in_tx], 1

.tx_open:
    xor ebx, ebx
    lea r13, [rip + g_batch_queue]

.batch_loop:
    cmp rbx, r12
    jge .commit_batch

    cmp qword ptr [r13 + 8], OP_CREATE_POST
    jne .do_like

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 1
    mov rdx, qword ptr [r13 + 16]
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 2
    lea rdx, [r13 + 160]
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .ins_failed

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    xor esi, esi
    call sqlite3_column_int64
    mov r14, rax

    mov rdi, qword ptr [rip + g_stmt_ins_post]
    mov esi, 1
    call sqlite3_column_text
    mov r15, rax

    lea rdi, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_post_prefix]
    mov qword ptr [rdi], rax

    lea rdi, [rip + g_json_buf + LEN_POST_PFX]
    mov rsi, r14
    lea rdx, [r13 + 160]
    mov rcx, r15
    lea r8, [r13 + 32]
    xor r9d, r9d
    call append_post_json
    mov word ptr [rax], 0x007d

    # wtf sqlite keeps vdbe open on RETURNING if it doesnt step to DONE before reset,
    # which makes COMMIT fail with SQLITE_BUSY and loses every write
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset

    mov qword ptr [r13 + 24], 201
    lea rdi, [r13 + 160]
    lea rsi, [rip + g_json_buf]
    call strcpy
    jmp .next_slot

.ins_failed:
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset
    mov qword ptr [r13 + 24], 500
    lea rdi, [r13 + 160]
    lea rsi, [rip + err_internal]
    call strcpy
    jmp .next_slot

.do_like:
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    mov esi, 1
    mov rdx, qword ptr [r13 + 16]
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    mov esi, 2
    mov rdx, qword ptr [r13 + 24]
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_step
    mov r15d, eax
    mov rdi, qword ptr [rip + g_stmt_ins_like]
    call sqlite3_reset
    cmp r15d, SQLITE_DONE
    jne .ins_failed

    mov rdi, qword ptr [rip + g_db]
    call sqlite3_changes
    mov r14, qword ptr [r13 + 24]
    test eax, eax
    jg .like_201

    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    mov esi, 1
    mov rdx, r14
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_step
    mov r15d, eax
    mov rdi, qword ptr [rip + g_stmt_post_exists]
    call sqlite3_reset
    cmp r15d, SQLITE_ROW
    jne .like_404

    mov qword ptr [r13 + 24], 200
    lea rdi, [r13 + 160]
    lea rsi, [rip + str_like_rep_pfx]
    mov edx, LEN_LIKE_REP
    call memcpy
    lea rdi, [r13 + 160 + LEN_LIKE_REP]
    mov rsi, r14
    call append_u64
    mov word ptr [rax], 0x007d
    jmp .next_slot

.like_201:
    mov qword ptr [r13 + 24], 201
    lea rdi, [r13 + 160]
    lea rsi, [rip + str_like_new_pfx]
    mov edx, LEN_LIKE_NEW
    call memcpy
    lea rdi, [r13 + 160 + LEN_LIKE_NEW]
    mov rsi, r14
    call append_u64
    mov word ptr [rax], 0x007d
    jmp .next_slot

.like_404:
    mov qword ptr [r13 + 24], 404
    lea rdi, [r13 + 160]
    lea rsi, [rip + err_post_not_found]
    call strcpy

.next_slot:
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .batch_loop

.commit_batch:
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 0
    cmp r14d, SQLITE_DONE
    jne .rollback_batch

    xor ebx, ebx
    lea r13, [rip + g_batch_queue]
.send_batch:
    cmp rbx, r12
    jge .maybe_ckpt
    mov edi, dword ptr [r13]
    mov esi, dword ptr [r13 + 24]
    lea rdx, [r13 + 160]
    call send_cstr_response
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .send_batch

.maybe_ckpt:
    inc qword ptr [rip + g_ckpt_counter]
    test qword ptr [rip + g_ckpt_counter], 63
    jnz .clear_batch
    mov rdi, qword ptr [rip + g_stmt_ckpt]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_ckpt]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_ckpt]
    call sqlite3_reset
    jmp .clear_batch

.rollback_batch:
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_rollback]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 0

.abort_batch:
    xor ebx, ebx
    lea r13, [rip + g_batch_queue]
.send_500_loop:
    cmp rbx, r12
    jge .clear_batch
    mov edi, dword ptr [r13]
    mov esi, 500
    lea rdx, [rip + err_internal]
    call send_cstr_response
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .send_500_loop

.clear_batch:
    mov qword ptr [rip + g_batch_count], 0

.fgc_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

handle_client_request:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24

    mov r12d, edi                   # fd
    mov r13, rsi                    # buf
    mov r14, rdx                    # len

    xor ebx, ebx
    mov rcx, r14
    sub rcx, 3
.scan_crlf:
    cmp rbx, rcx
    jge .ret_404
    cmp dword ptr [r13 + rbx], 0x0a0d0a0d
    je .got_headers
    inc rbx
    jmp .scan_crlf

.got_headers:
    add rbx, 4
    mov rbp, rbx                    # headers_len in rbp

    cmp dword ptr [r13], 0x20544547 # "GET "
    je .route_get
    cmp dword ptr [r13], 0x54534f50 # "POST"
    jne .ret_404
    cmp byte ptr [r13 + 4], ' '
    jne .ret_404

    lea r15, [r13 + 5]
    xor ecx, ecx
1:
    movzx eax, byte ptr [r15 + rcx]
    cmp al, ' '
    je .route_post
    cmp al, '?'
    je .route_post
    cmp al, 13
    je .route_post
    test al, al
    jz .ret_404
    inc rcx
    jmp 1b

.route_post:
    mov qword ptr [rsp + 8], rcx
    cmp rcx, 6
    jne .try_post_like
    cmp dword ptr [r15], 0x736f702f # "/pos"
    jne .ret_404
    cmp word ptr [r15 + 4], 0x7374  # "ts"
    jne .ret_404

    mov rdi, r13
    mov rsi, rbp
    call find_bearer_token
    test rax, rax
    jz .ret_401_missing

    mov rdi, rax
    mov rsi, rdx
    call verify_jwt
    cmp rax, -1
    je .ret_401_invalid
    cmp rax, -2
    je .ret_401_payload

    lea rdi, [r13 + rbp]
    mov rsi, r14
    sub rsi, rbp
    mov byte ptr [rdi + rsi], 0

    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_val_body]
    mov esi, 1
    lea rdx, [r13 + rbp]
    mov ecx, -1
    xor r8d, r8d
    call sqlite3_bind_text

    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .bad_json_reset

    mov rdi, qword ptr [rip + g_stmt_val_body]
    xor esi, esi
    call sqlite3_column_int64
    mov r14, rax
    cmp r14, -1
    je .bad_json_reset
    cmp r14, -2
    je .req_body_reset
    test r14, r14
    jle .req_body_reset
    cmp r14, 500
    jg .long_body_reset

    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12
    mov qword ptr [r15 + 8], OP_CREATE_POST
    mov rax, qword ptr [rip + g_auth_uid]
    mov qword ptr [r15 + 16], rax

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
    jmp .req_done

.bad_json_reset:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .ret_400_malformed

.req_body_reset:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .ret_400_required

.long_body_reset:
    mov rdi, qword ptr [rip + g_stmt_val_body]
    call sqlite3_reset
    jmp .ret_400_too_long

.try_post_like:
    cmp rcx, 12
    jle .ret_404
    cmp dword ptr [r15], 0x736f702f
    jne .ret_404
    cmp word ptr [r15 + 4], 0x7374
    jne .ret_404
    cmp byte ptr [r15 + 6], '/'
    jne .ret_404

    lea rax, [r15 + rcx - 5]
    cmp dword ptr [rax], 0x6b696c2f # "/lik"
    jne .ret_404
    cmp byte ptr [rax + 4], 'e'
    jne .ret_404

    # spec says check auth before post id or test.sh cries on /posts/abc/like
    mov rdi, r13
    mov rsi, rbp
    call find_bearer_token
    test rax, rax
    jz .ret_401_missing

    mov rdi, rax
    mov rsi, rdx
    call verify_jwt
    cmp rax, -1
    je .ret_401_invalid
    cmp rax, -2
    je .ret_401_payload

    lea rdi, [r15 + 7]
    mov rsi, qword ptr [rsp + 8]
    sub rsi, 12
    call parse_positive_int
    test rax, rax
    jle .ret_400_invalid_id

    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12
    mov qword ptr [r15 + 8], OP_LIKE_POST
    mov rcx, qword ptr [rip + g_auth_uid]
    mov qword ptr [r15 + 16], rcx
    mov qword ptr [r15 + 24], rax

    inc qword ptr [rip + g_batch_count]
    jmp .req_done

.route_get:
    lea r15, [r13 + 4]
    xor ecx, ecx
2:
    movzx eax, byte ptr [r15 + rcx]
    cmp al, ' '
    je .dispatch_get
    cmp al, '?'
    je .dispatch_get
    cmp al, 13
    je .dispatch_get
    test al, al
    jz .ret_404
    inc rcx
    jmp 2b

.dispatch_get:
    cmp rcx, 5
    jne .try_health
    cmp dword ptr [r15], 0x6565662f # "/fee"
    jne .ret_404
    cmp byte ptr [r15 + 4], 'd'
    jne .ret_404

    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_reset

    lea rbx, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_feed_prefix]
    mov qword ptr [rbx], rax
    mov word ptr [rbx + 8], 0x5b3a  # ":["
    add rbx, LEN_FEED_PFX
    xor r14d, r14d

.feed_next_row:
    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .feed_done

    test r14d, r14d
    jz 3f
    mov byte ptr [rbx], ','
    inc rbx
3:
    inc r14d

    mov rdi, qword ptr [rip + g_stmt_feed]
    xor esi, esi
    call sqlite3_column_int64
    mov r15, rax

    mov rdi, qword ptr [rip + g_stmt_feed]
    mov esi, 1
    call sqlite3_column_text
    mov qword ptr [rsp], rax

    mov rdi, qword ptr [rip + g_stmt_feed]
    mov esi, 2
    call sqlite3_column_text
    mov qword ptr [rsp + 8], rax

    mov rdi, qword ptr [rip + g_stmt_feed]
    mov esi, 3
    call sqlite3_column_text
    mov qword ptr [rsp + 16], rax

    mov rdi, qword ptr [rip + g_stmt_feed]
    mov esi, 4
    call sqlite3_column_int64
    mov r9, rax

    mov rdi, rbx
    mov rsi, r15
    mov rdx, qword ptr [rsp]
    mov rcx, qword ptr [rsp + 8]
    mov r8, qword ptr [rsp + 16]
    call append_post_json
    mov rbx, rax
    jmp .feed_next_row

.feed_done:
    mov word ptr [rbx], 0x7d5d
    add rbx, 2

    mov rdi, qword ptr [rip + g_stmt_feed]
    call sqlite3_reset

    lea rdx, [rip + g_json_buf]
    mov rcx, rbx
    sub rcx, rdx
    mov edi, r12d
    mov esi, 200
    call send_http_response
    jmp .req_done

.try_health:
    cmp rcx, 7
    jne .try_get_post
    cmp dword ptr [r15], 0x6165682f # "/hea"
    jne .ret_404
    cmp word ptr [r15 + 4], 0x746c
    jne .ret_404
    cmp byte ptr [r15 + 6], 'h'
    jne .ret_404

    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_step
    mov ebx, eax
    mov rdi, qword ptr [rip + g_stmt_health]
    call sqlite3_reset
    cmp ebx, SQLITE_ROW
    jne .ret_503

    lea rdi, [rip + g_json_buf]
    lea rsi, [rip + str_health_prefix]
    mov edx, LEN_HEALTH_PFX
    call memcpy

    xor edi, edi
    call time
    sub rax, qword ptr [rip + g_start_time]
    lea rdi, [rip + g_json_buf + LEN_HEALTH_PFX]
    mov rsi, rax
    call append_u64
    mov byte ptr [rax], '}'
    inc rax

    lea rdx, [rip + g_json_buf]
    mov rcx, rax
    sub rcx, rdx
    mov edi, r12d
    mov esi, 200
    call send_http_response
    jmp .req_done

.try_get_post:
    cmp rcx, 7
    jle .ret_404
    cmp dword ptr [r15], 0x736f702f
    jne .ret_404
    cmp word ptr [r15 + 4], 0x7374
    jne .ret_404
    cmp byte ptr [r15 + 6], '/'
    jne .ret_404

    lea rdi, [r15 + 7]
    mov rsi, rcx
    sub rsi, 7
    call parse_positive_int
    test rax, rax
    jle .ret_400_invalid_id

    mov rbx, rax
    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 1
    mov rdx, rbx
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .post_missing

    lea rdi, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_post_prefix]
    mov qword ptr [rdi], rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    xor esi, esi
    call sqlite3_column_int64
    mov r15, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 1
    call sqlite3_column_text
    mov qword ptr [rsp], rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 2
    call sqlite3_column_text
    mov qword ptr [rsp + 8], rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 3
    call sqlite3_column_text
    mov qword ptr [rsp + 16], rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 4
    call sqlite3_column_int64
    mov r9, rax

    lea rdi, [rip + g_json_buf + LEN_POST_PFX]
    mov rsi, r15
    mov rdx, qword ptr [rsp]
    mov rcx, qword ptr [rsp + 8]
    mov r8, qword ptr [rsp + 16]
    call append_post_json
    mov byte ptr [rax], '}'
    inc rax
    mov rbx, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset

    lea rdx, [rip + g_json_buf]
    mov rcx, rbx
    sub rcx, rdx
    mov edi, r12d
    mov esi, 200
    call send_http_response
    jmp .req_done

.post_missing:
    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_reset
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_post_not_found]
    call send_cstr_response
    jmp .req_done

.ret_404:
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_not_found]
    call send_cstr_response
    jmp .req_done

.ret_400_invalid_id:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_invalid_id]
    call send_cstr_response
    jmp .req_done

.ret_400_malformed:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_malformed_json]
    call send_cstr_response
    jmp .req_done

.ret_400_required:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_required]
    call send_cstr_response
    jmp .req_done

.ret_400_too_long:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_too_long]
    call send_cstr_response
    jmp .req_done

.ret_401_missing:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_missing_token]
    call send_cstr_response
    jmp .req_done

.ret_401_invalid:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_token]
    call send_cstr_response
    jmp .req_done

.ret_401_payload:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_pay]
    call send_cstr_response
    jmp .req_done

.ret_503:
    mov edi, r12d
    mov esi, 503
    lea rdx, [rip + err_degraded]
    call send_cstr_response
    jmp .req_done

.ret_500:
    mov edi, r12d
    mov esi, 500
    lea rdx, [rip + err_internal]
    call send_cstr_response

.req_done:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

# rsi = sql, r8 = &stmt_out
prep_stmt:
    mov rdi, qword ptr [rip + g_db]
    mov edx, -1
    mov ecx, SQLITE_PREPARE_PERSIST
    xor r9d, r9d
    jmp sqlite3_prepare_v3

# r12d = listen_fd, r13d = epoll_fd
drain_accept:
    push rbx
    sub rsp, 16
.acc_next:
    mov eax, SYS_ACCEPT4
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov r10d, SOCK_NONBLOCK_CLOEXEC
    syscall
    test eax, eax
    js .acc_done
    mov ebx, eax
    mov dword ptr [rsp], 1
    mov edi, ebx
    mov esi, IPPROTO_TCP
    mov edx, TCP_NODELAY
    mov rcx, rsp
    mov r8d, 4
    call setsockopt
    lea r10, [rip + g_epoll_ev]
    mov dword ptr [r10], EPOLLIN
    mov dword ptr [r10 + 4], ebx
    mov dword ptr [r10 + 8], 0
    mov eax, SYS_EPOLL_CTL
    mov edi, r13d
    mov esi, EPOLL_CTL_ADD
    mov edx, ebx
    syscall
    jmp .acc_next
.acc_done:
    add rsp, 16
    pop rbx
    ret

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
    jnz 1f
    lea rax, [rip + def_jwt_secret]
1:
    mov qword ptr [rip + g_jwt_secret], rax
    mov rdi, rax
    call strlen
    mov qword ptr [rip + g_jwt_secret_len], rax

    lea rdi, [rip + env_sqlite_path]
    call getenv
    test rax, rax
    jnz 2f
    lea rax, [rip + def_sqlite_path]
2:
    mov rdi, rax
    lea rsi, [rip + g_db]
    mov edx, SQLITE_OPEN_RW_NOMUTEX
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_pragmas]
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec

    lea rsi, [rip + sql_health]
    lea r8, [rip + g_stmt_health]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_feed]
    lea r8, [rip + g_stmt_feed]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_get_post]
    lea r8, [rip + g_stmt_get_post]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_validate_body]
    lea r8, [rip + g_stmt_val_body]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_insert_post]
    lea r8, [rip + g_stmt_ins_post]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_post_exists]
    lea r8, [rip + g_stmt_post_exists]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_insert_like]
    lea r8, [rip + g_stmt_ins_like]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_begin_read]
    lea r8, [rip + g_stmt_begin_read]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_begin]
    lea r8, [rip + g_stmt_begin]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_commit]
    lea r8, [rip + g_stmt_commit]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_rollback]
    lea r8, [rip + g_stmt_rollback]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_ckpt]
    lea r8, [rip + g_stmt_ckpt]
    call prep_stmt
    test eax, eax
    jnz .fatal

    mov edi, AF_INET
    mov esi, SOCK_STREAM_NONBLOCK
    xor edx, edx
    call socket
    test eax, eax
    js .fatal
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
    jnz 3f
    lea rax, [rip + def_port]
3:
    mov rdi, rax
    call atoi
    rol ax, 8
    mov word ptr [r13 + 2], ax

    lea rdi, [rip + env_host]
    call getenv
    test rax, rax
    jnz 4f
    lea rax, [rip + def_host]
4:
    mov rdi, rax
    call inet_addr
    mov dword ptr [r13 + 4], eax

    mov edi, r12d
    mov rsi, r13
    mov edx, 16
    call bind
    test eax, eax
    jnz .fatal

    mov edi, r12d
    mov esi, 65535
    call listen
    test eax, eax
    jnz .fatal

    mov eax, SYS_EPOLL_CREATE1
    xor edi, edi
    syscall
    test eax, eax
    js .fatal
    mov qword ptr [rip + g_epoll_fd], rax
    mov r13d, eax

    lea r10, [rip + g_epoll_ev]
    mov dword ptr [r10], EPOLLIN
    mov qword ptr [r10 + 4], r12
    mov eax, SYS_EPOLL_CTL
    mov edi, r13d
    mov esi, EPOLL_CTL_ADD
    mov edx, r12d
    syscall

    mov eax, SYS_WRITE
    mov edi, 2
    lea rsi, [rip + msg_up]
    mov edx, LEN_MSG_UP
    syscall

.ev_loop:
    mov eax, SYS_EPOLL_WAIT
    mov edi, r13d
    lea rsi, [rip + g_epoll_events]
    mov edx, MAX_EVENTS
    mov r10d, -1
    syscall
    test eax, eax
    jle .ev_loop

    mov r14d, eax
    xor r15d, r15d

    cmp r14d, 4
    jl .next_ev
    cmp qword ptr [rip + g_in_tx], 0
    jne .next_ev
    mov rdi, qword ptr [rip + g_stmt_begin_read]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_begin_read]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_begin_read]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 1

.next_ev:
    cmp r15d, r14d
    jge .tick_done

    test r15d, 127
    jnz .handle_ev
    test r15d, r15d
    jz .handle_ev
    call drain_accept

.handle_ev:
    imul ebx, r15d, 12
    lea rcx, [rip + g_epoll_events]
    mov eax, dword ptr [rcx + rbx + 4]
    cmp eax, r12d
    jne .client_io
    call drain_accept
    jmp .advance_ev

.client_io:
    mov ebx, eax
    xor eax, eax                    # SYS_READ
    mov edi, ebx
    lea rsi, [rip + g_req_buf]
    mov edx, 32000
    syscall
    test rax, rax
    jle .drop_client

    mov rdx, rax
    lea rsi, [rip + g_req_buf]
    mov byte ptr [rsi + rdx], 0

    mov edi, ebx
    call handle_client_request

    cmp qword ptr [rip + g_batch_count], MAX_BATCH
    jl .advance_ev
    call flush_group_commit
    jmp .advance_ev

.drop_client:
    mov eax, SYS_EPOLL_CTL
    mov edi, r13d
    mov esi, EPOLL_CTL_DEL
    mov edx, ebx
    xor r10d, r10d
    syscall
    mov eax, SYS_CLOSE
    mov edi, ebx
    syscall

.advance_ev:
    inc r15d
    jmp .next_ev

.tick_done:
    cmp qword ptr [rip + g_batch_count], 0
    jg .do_flush

    cmp qword ptr [rip + g_in_tx], 0
    je .ev_loop
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 0
    jmp .ev_loop

.do_flush:
    call flush_group_commit
    jmp .ev_loop

.fatal:
    mov eax, SYS_WRITE
    mov edi, 2
    lea rsi, [rip + msg_fatal]
    mov edx, LEN_MSG_FATAL
    syscall
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