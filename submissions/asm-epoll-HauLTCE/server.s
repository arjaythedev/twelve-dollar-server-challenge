# 12$ server challenge - x86_64 linux asm (gas, intel syntax)
# gcc -no-pie -O3 -march=native server.s -o bin/server -lsqlite3 -lcrypto

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
.extern sqlite3_close_v2
.extern sqlite3_exec
.extern sqlite3_prepare_v3
.extern sqlite3_reset
.extern sqlite3_bind_int64
.extern sqlite3_bind_text
.extern sqlite3_step
.extern sqlite3_column_int64
.extern sqlite3_column_text
.extern sqlite3_column_bytes
.extern sqlite3_changes

.extern SHA256_Init
.extern SHA256_Update
.extern SHA256_Final
.extern CRYPTO_memcmp

.equ SYS_READ,               0
.equ SYS_WRITE,              1
.equ SYS_CLOSE,              3
.equ SYS_FORK,               57
.equ SYS_GETRLIMIT,          97
.equ SYS_EPOLL_WAIT,         232
.equ SYS_EPOLL_CTL,          233
.equ SYS_ACCEPT4,            288
.equ SYS_EVENTFD2,           290
.equ SYS_EPOLL_CREATE1,      291

.equ RLIMIT_NOFILE,          7
.equ AF_INET,                2
.equ SOCK_STREAM_NONBLOCK,   0x80801
.equ SOCK_NONBLOCK_CLOEXEC,  0x80800
.equ SOL_SOCKET,             1
.equ SO_REUSEADDR,           2
.equ SO_REUSEPORT,           15
.equ EPOLL_CTL_ADD,          1
.equ EPOLL_CTL_DEL,          2
.equ EPOLLIN,                1
.equ MAX_EVENTS,             512
.equ MAX_BATCH,              256
.equ DEF_FD_LIMIT,           63500
.equ MAX_WORKERS,            6
.equ SQLITE_OPEN_RW_NOMUTEX, 0x00008002
.equ SQLITE_PREPARE_PERSIST, 1
.equ SQLITE_BUSY,            5
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
env_fd_limit:       .asciz "FD_LIMIT"

msg_up:             .ascii "[asm] listening\n"
.equ LEN_MSG_UP,    . - msg_up
msg_fatal:          .ascii "fatal: init failed\n"
.equ LEN_MSG_FATAL, . - msg_fatal

# worker 0 runs full index warmup; standby workers run fast pragma init when woken up
sql_pragmas_w0:
    .ascii "PRAGMA journal_mode = WAL;"
    .ascii "PRAGMA busy_timeout = 5000;"
    .ascii "PRAGMA synchronous = NORMAL;"
    .ascii "PRAGMA temp_store = MEMORY;"
    .ascii "PRAGMA mmap_size = 1073741824;"
    .ascii "PRAGMA cache_size = -16000;"
    .ascii "PRAGMA wal_autocheckpoint = 0;"
    .ascii "PRAGMA journal_size_limit = 67108864;"
    .ascii "SELECT count(*) FROM likes INDEXED BY likes_post_id_idx;"
    .ascii "SELECT count(*) FROM users;"
    .asciz "SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx;"

sql_pragmas_wn:
    .ascii "PRAGMA busy_timeout = 5000;"
    .ascii "PRAGMA synchronous = NORMAL;"
    .ascii "PRAGMA temp_store = MEMORY;"
    .ascii "PRAGMA mmap_size = 536870912;"
    .ascii "PRAGMA cache_size = -4000;"
    .ascii "PRAGMA wal_autocheckpoint = 0;"
    .asciz "PRAGMA journal_size_limit = 67108864;"

sql_busy_zero:
    .asciz "PRAGMA busy_timeout = 0;"

sql_health:
    .asciz "SELECT 1"

sql_feed_ids:
    .asciz "SELECT id FROM posts INDEXED BY posts_created_at_id_idx ORDER BY created_at DESC, id DESC LIMIT 20"

sql_feed_likes:
    .asciz "SELECT post_id FROM likes INDEXED BY likes_post_id_idx WHERE post_id >= ?1 AND post_id <= ?2"

sql_feed_range:
    .asciz "SELECT p.id, p.body, p.created_at, u.username FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id >= ?1 AND p.id <= ?2 ORDER BY p.id DESC"

sql_feed_slow:
    .ascii "SELECT p.id, p.body, p.created_at, u.username, "
    .ascii "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    .ascii "FROM posts p JOIN users u ON u.id = p.user_id "
    .asciz "ORDER BY p.created_at DESC, p.id DESC LIMIT 20"

sql_get_post:
    .ascii "SELECT p.id, p.body, p.created_at, u.username, "
    .ascii "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "
    .asciz "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1"

sql_chk_like:
    .ascii "SELECT CASE "
    .ascii "  WHEN EXISTS (SELECT 1 FROM likes WHERE user_id = ?1 AND post_id = ?2) THEN 2 "
    .ascii "  WHEN EXISTS (SELECT 1 FROM posts WHERE id = ?2) THEN 1 "
    .ascii "  ELSE 0 "
    .asciz "END"

# fallback for escaped/unicode/weird json on POST /posts
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

sql_insert_like:
    .asciz "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING"

sql_begin_read:     .asciz "BEGIN"
sql_begin:          .asciz "BEGIN IMMEDIATE"
sql_commit:         .asciz "COMMIT"
sql_rollback:       .asciz "ROLLBACK"
sql_ckpt:           .asciz "PRAGMA wal_checkpoint(PASSIVE)"

err_not_found:      .asciz "{\"error\":\"not found\"}"
.equ LEN_ERR_NF,    . - err_not_found - 1
err_invalid_id:     .asciz "{\"error\":\"invalid post id\"}"
.equ LEN_ERR_ID,    . - err_invalid_id - 1
err_post_not_found: .asciz "{\"error\":\"post not found\"}"
.equ LEN_ERR_PNF,   . - err_post_not_found - 1
err_missing_token:  .asciz "{\"error\":\"missing bearer token\"}"
.equ LEN_ERR_MTOK,  . - err_missing_token - 1
err_invalid_token:  .asciz "{\"error\":\"invalid or expired token\"}"
.equ LEN_ERR_ITOK,  . - err_invalid_token - 1
err_invalid_pay:    .asciz "{\"error\":\"invalid token payload\"}"
.equ LEN_ERR_IPAY,  . - err_invalid_pay - 1
err_malformed_json: .asciz "{\"error\":\"malformed JSON body\"}"
.equ LEN_ERR_MAL,   . - err_malformed_json - 1
err_body_required:  .asciz "{\"error\":\"body is required\"}"
.equ LEN_ERR_REQ,   . - err_body_required - 1
err_body_too_long:  .asciz "{\"error\":\"body must be at most 500 characters\"}"
.equ LEN_ERR_LONG,  . - err_body_too_long - 1
err_internal:       .asciz "{\"error\":\"internal server error\"}"
.equ LEN_ERR_INT,   . - err_internal - 1
err_degraded:       .asciz "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"db failed\"}"
.equ LEN_ERR_DEG,   . - err_degraded - 1

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
g_now_time:         .quad 0
g_db_path:          .quad 0
g_db:               .quad 0
g_jwt_secret:       .quad 0
g_jwt_secret_len:   .quad 0
g_listen_fd:        .quad 0
g_epoll_fd:         .quad 0
g_fd_limit:         .quad 0
g_worker_idx:       .quad 0
g_next_efd:         .quad 0
g_handed_off:       .quad 0
g_in_tx:            .quad 0
g_tick_evs:         .quad 0
g_ckpt_counter:     .quad 0
g_coalesced:        .quad 0

g_stmt_health:      .quad 0
g_stmt_feed_ids:    .quad 0
g_stmt_feed_likes:  .quad 0
g_stmt_feed_range:  .quad 0
g_stmt_feed_slow:   .quad 0
g_stmt_get_post:    .quad 0
g_stmt_chk_like:    .quad 0
g_stmt_val_body:    .quad 0
g_stmt_ins_post:    .quad 0
g_stmt_ins_like:    .quad 0
g_stmt_begin_read:  .quad 0
g_stmt_begin:       .quad 0
g_stmt_commit:      .quad 0
g_stmt_rollback:    .quad 0
g_stmt_ckpt:        .quad 0

g_auth_uid:         .quad 0
g_auth_username:    .space 256

# pre-keyed HMAC-SHA256 contexts (112 bytes each) + working context
.align 64
g_hmac_ipad_ctx:    .space 128
g_hmac_opad_ctx:    .space 128
g_sha_work_ctx:     .space 128
g_hmac_k_ipad:      .space 64
g_hmac_k_opad:      .space 64

# 32-slot like count array for contiguous 20-post range scan
.align 64
g_like_counts:      .space (32 * 8)

g_sockaddr:         .space 16
g_epoll_ev:         .space 16
g_epoll_events:     .space (MAX_EVENTS * 12)
g_req_buf:          .space 32768
g_resp_buf:         .space 65536
g_resp_200_buf:     .space 65536
g_json_buf:         .space 65536
g_num_tmp:          .space 32
g_jwt_hdr_buf:      .space 2048
g_jwt_pay_buf:      .space 4096
g_jwt_sig_buf:      .space 256
g_hmac_out:         .space 64

g_batch_count:      .quad 0
g_batch_queue:      .space (MAX_BATCH * BATCH_ITEM_SIZE)

.section .text

# open shared read tx once per multi-event epoll batch
ensure_read_tx:
    cmp qword ptr [rip + g_in_tx], 0
    jne .ert_done
    cmp qword ptr [rip + g_tick_evs], 2
    jl .ert_done
    mov rdi, qword ptr [rip + g_stmt_begin_read]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_begin_read]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 1
.ert_done:
    ret

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

# only calls leaf helpers above, so r10/r11 survive
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

    cmp r12d, dword ptr [rip + g_fd_limit]
    jg .use_close_hdr

    cmp eax, 200
    je .fast_200_ka

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

.fast_200_ka:
    lea rdi, [rip + g_resp_200_buf + LEN_200_KA]
    mov rsi, r11
    call append_u64
    mov dword ptr [rax], 0x0a0d0a0d
    add rax, 4
    lea rcx, [rip + g_resp_200_buf]
    mov rbx, rax
    sub rbx, rcx
    mov rdi, rax
    mov rsi, r10
    mov rcx, r11
    rep movsb
    add rbx, r11
    lea r10, [rip + g_resp_200_buf]
    jmp .write_again

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
    cmp r12d, dword ptr [rip + g_fd_limit]
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

# keep bit count in cl and accum in r11d so the loop never touches the stack
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

# precompute SHA256 ipad/opad contexts from g_jwt_secret once at startup
init_hmac_ctx:
    push rbx
    sub rsp, 16

    mov rax, 0x3636363636363636
    lea rdi, [rip + g_hmac_k_ipad]
    mov ecx, 8
    rep stosq

    mov rax, 0x5c5c5c5c5c5c5c5c
    lea rdi, [rip + g_hmac_k_opad]
    mov ecx, 8
    rep stosq

    mov rsi, qword ptr [rip + g_jwt_secret]
    mov rcx, qword ptr [rip + g_jwt_secret_len]
    cmp rcx, 64
    jle .xor_key

    # RFC 2104: if secret > 64 bytes, hash it with SHA256 into 32 bytes first
    lea rdi, [rip + g_sha_work_ctx]
    call SHA256_Init
    lea rdi, [rip + g_sha_work_ctx]
    mov rsi, qword ptr [rip + g_jwt_secret]
    mov rdx, qword ptr [rip + g_jwt_secret_len]
    call SHA256_Update
    lea rdi, [rip + g_hmac_out]
    lea rsi, [rip + g_sha_work_ctx]
    call SHA256_Final
    lea rsi, [rip + g_hmac_out]
    mov ecx, 32

.xor_key:
    lea r8, [rip + g_hmac_k_ipad]
    lea r9, [rip + g_hmac_k_opad]
    xor edx, edx
1:
    cmp rdx, rcx
    jge 2f
    movzx eax, byte ptr [rsi + rdx]
    xor byte ptr [r8 + rdx], al
    xor byte ptr [r9 + rdx], al
    inc rdx
    jmp 1b
2:
    lea rdi, [rip + g_hmac_ipad_ctx]
    call SHA256_Init
    lea rdi, [rip + g_hmac_ipad_ctx]
    lea rsi, [rip + g_hmac_k_ipad]
    mov edx, 64
    call SHA256_Update

    lea rdi, [rip + g_hmac_opad_ctx]
    call SHA256_Init
    lea rdi, [rip + g_hmac_opad_ctx]
    lea rsi, [rip + g_hmac_k_opad]
    mov edx, 64
    call SHA256_Update

    add rsp, 16
    pop rbx
    ret

# verifies every token's HMAC-SHA256 signature + claims on every request using pre-keyed SHA256 contexts
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

    # inner hash: SHA256(k_ipad || header.payload)
    lea rdi, [rip + g_sha_work_ctx]
    lea rsi, [rip + g_hmac_ipad_ctx]
    mov ecx, 14
    rep movsq
    lea rdi, [rip + g_sha_work_ctx]
    mov rsi, r12
    mov rdx, r15
    call SHA256_Update
    lea rdi, [rip + g_hmac_out]
    lea rsi, [rip + g_sha_work_ctx]
    call SHA256_Final

    # outer hash: SHA256(k_opad || inner_hash)
    lea rdi, [rip + g_sha_work_ctx]
    lea rsi, [rip + g_hmac_opad_ctx]
    mov ecx, 14
    rep movsq
    lea rdi, [rip + g_sha_work_ctx]
    lea rsi, [rip + g_hmac_out]
    mov edx, 32
    call SHA256_Update
    lea rdi, [rip + g_hmac_out]
    lea rsi, [rip + g_sha_work_ctx]
    call SHA256_Final

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
    mov rax, qword ptr [rip + g_now_time]
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
    cmp ecx, 60
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

    # close any open read snapshot before starting BEGIN IMMEDIATE
    cmp qword ptr [rip + g_in_tx], 0
    je .start_write_tx
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 0

.start_write_tx:
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_step
    mov r14d, eax
    mov rdi, qword ptr [rip + g_stmt_begin]
    call sqlite3_reset
    cmp r14d, SQLITE_BUSY
    je .fgc_done                    # another worker holds write lock; keep batch queued for next tick
    cmp r14d, SQLITE_DONE
    jne .abort_batch
    mov qword ptr [rip + g_in_tx], 1

    xor ebx, ebx
    lea r13, [rip + g_batch_queue]

.batch_loop:
    cmp rbx, r12
    jge .commit_batch

    cmp qword ptr [r13 + 8], OP_CREATE_POST
    jne .do_like

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
    mov byte ptr [rax], '}'
    inc rax
    lea rsi, [rip + g_json_buf]
    mov rcx, rax
    sub rcx, rsi
    mov qword ptr [r13 + 8], rcx
    lea rdi, [r13 + 160]
    rep movsb

    # wtf sqlite keeps vdbe open on RETURNING if i dont step to DONE before reset,
    # which makes COMMIT fail with SQLITE_BUSY and drops the whole batch
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset

    mov qword ptr [r13 + 24], 201
    jmp .next_slot

.ins_failed:
    mov rdi, qword ptr [rip + g_stmt_ins_post]
    call sqlite3_reset
    mov qword ptr [r13 + 24], 500
    mov qword ptr [r13 + 8], LEN_ERR_INT
    lea rdi, [r13 + 160]
    lea rsi, [rip + err_internal]
    mov ecx, LEN_ERR_INT
    rep movsb
    jmp .next_slot

.do_like:
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

    mov qword ptr [r13 + 24], 200
    lea rdi, [r13 + 160]
    lea rsi, [rip + str_like_rep_pfx]
    mov ecx, LEN_LIKE_REP
    rep movsb
    lea rdi, [r13 + 160 + LEN_LIKE_REP]
    mov rsi, r14
    call append_u64
    mov byte ptr [rax], '}'
    inc rax
    lea rcx, [r13 + 160]
    sub rax, rcx
    mov qword ptr [r13 + 8], rax
    jmp .next_slot

.like_201:
    mov qword ptr [r13 + 24], 201
    lea rdi, [r13 + 160]
    lea rsi, [rip + str_like_new_pfx]
    mov ecx, LEN_LIKE_NEW
    rep movsb
    lea rdi, [r13 + 160 + LEN_LIKE_NEW]
    mov rsi, r14
    call append_u64
    mov byte ptr [rax], '}'
    inc rax
    lea rcx, [r13 + 160]
    sub rax, rcx
    mov qword ptr [r13 + 8], rax

.next_slot:
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .batch_loop

.commit_batch:
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
    mov rcx, qword ptr [r13 + 8]
    call send_http_response
    inc rbx
    add r13, BATCH_ITEM_SIZE
    jmp .send_batch

.maybe_ckpt:
    inc qword ptr [rip + g_ckpt_counter]
    test qword ptr [rip + g_ckpt_counter], 511
    jnz .clear_batch
    mov rdi, qword ptr [rip + g_stmt_ckpt]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_ckpt]
    call sqlite3_reset
    jmp .clear_batch

.rollback_batch:
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
    mov ecx, LEN_ERR_INT
    call send_http_response
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

    # dispatch GET right away in registers without scanning headers for \r\n\r\n
    cmp r14, 14
    jl .ret_404
    cmp dword ptr [r13], 0x20544547 # "GET "
    je .route_get

    cmp dword ptr [r13], 0x54534f50 # "POST"
    jne .ret_404
    cmp byte ptr [r13 + 4], ' '
    jne .ret_404

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

    # fast path for clean ascii {"body":"..."} so k6/wrk skips sqlite json_extract
    cmp rsi, 11
    jl .val_body_sqlite
    cmp dword ptr [rdi], 0x6f62227b     # {"bo
    jne .val_body_sqlite
    cmp dword ptr [rdi + 4], 0x223a2264 # dy":
    jne .val_body_sqlite
    cmp byte ptr [rdi + 8], '"'
    jne .val_body_sqlite

    lea r8, [rdi + 9]
    xor ecx, ecx
.fp_scan_q:
    movzx eax, byte ptr [r8 + rcx]
    test al, al
    jz .val_body_sqlite
    cmp al, '"'
    je .fp_got_q
    cmp al, '\\'
    je .val_body_sqlite
    cmp al, 32
    jl .val_body_sqlite
    cmp al, 126
    jg .val_body_sqlite
    inc rcx
    cmp rcx, 505
    jg .val_body_sqlite
    jmp .fp_scan_q

.fp_got_q:
    cmp byte ptr [r8 + rcx + 1], '}'
    jne .val_body_sqlite
    cmp byte ptr [r8 + rcx + 2], 0
    jne .val_body_sqlite

    # trim ascii spaces on both ends
    xor edx, edx
.fp_trim_l:
    cmp rdx, rcx
    jge .val_body_sqlite
    cmp byte ptr [r8 + rdx], ' '
    jne .fp_trim_r
    inc rdx
    jmp .fp_trim_l
.fp_trim_r:
    cmp rcx, rdx
    jle .val_body_sqlite
    cmp byte ptr [r8 + rcx - 1], ' '
    jne .fp_trimmed
    dec rcx
    jmp .fp_trim_r
.fp_trimmed:
    sub rcx, rdx
    test rcx, rcx
    jle .val_body_sqlite
    cmp rcx, 500
    jg .val_body_sqlite

    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12
    mov qword ptr [r15 + 8], OP_CREATE_POST
    mov rax, qword ptr [rip + g_auth_uid]
    mov qword ptr [r15 + 16], rax

    lea rdi, [r15 + 160]
    lea rsi, [r8 + rdx]
    mov r9, rcx
    rep movsb
    mov byte ptr [r15 + 160 + r9], 0

    lea rdi, [r15 + 32]
    lea rsi, [rip + g_auth_username]
    mov ecx, 8
    rep movsq

    inc qword ptr [rip + g_batch_count]
    jmp .req_done

.val_body_sqlite:
    call ensure_read_tx
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
    mov ecx, 8
    rep movsq

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
    mov r14, rax                    # post_id

    # query sqlite first: 2 = already liked, 1 = needs insert, 0 = post missing
    call ensure_read_tx
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    mov esi, 1
    mov rdx, qword ptr [rip + g_auth_uid]
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    mov esi, 2
    mov rdx, r14
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .chk_like_err
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    xor esi, esi
    call sqlite3_column_int64
    mov rbx, rax
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    call sqlite3_reset

    test rbx, rbx
    jz .like_not_found
    cmp rbx, 2
    je .like_already_done

    mov rcx, qword ptr [rip + g_auth_uid]
    mov rbx, qword ptr [rip + g_batch_count]
    imul rbx, rbx, BATCH_ITEM_SIZE
    lea r15, [rip + g_batch_queue]
    add r15, rbx

    mov qword ptr [r15], r12
    mov qword ptr [r15 + 8], OP_LIKE_POST
    mov qword ptr [r15 + 16], rcx
    mov qword ptr [r15 + 24], r14

    inc qword ptr [rip + g_batch_count]
    jmp .req_done

.chk_like_err:
    mov rdi, qword ptr [rip + g_stmt_chk_like]
    call sqlite3_reset
    jmp .ret_500

.like_not_found:
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_post_not_found]
    mov ecx, LEN_ERR_PNF
    call send_http_response
    jmp .req_done

.like_already_done:
    lea rdi, [rip + g_json_buf]
    lea rsi, [rip + str_like_rep_pfx]
    mov ecx, LEN_LIKE_REP
    rep movsb
    lea rdi, [rip + g_json_buf + LEN_LIKE_REP]
    mov rsi, r14
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

.route_get:
    mov rax, 0x6565662f20544547     # "GET /fee"
    cmp qword ptr [r13], rax
    jne .try_get_post
    cmp byte ptr [r13 + 8], 'd'
    jne .ret_404
    movzx eax, byte ptr [r13 + 9]
    cmp al, ' '
    je .do_get_feed
    cmp al, '?'
    je .do_get_feed
    jmp .ret_404

.do_get_feed:
    call ensure_read_tx

    # step 1: check if newest 20 post ids from covering index are contiguous (max_id .. max_id - 19)
    xor ebx, ebx                    # row count
    xor r14d, r14d                  # max_id
    mov r15d, 1                     # contiguous flag
.feed_id_loop:
    mov rdi, qword ptr [rip + g_stmt_feed_ids]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .feed_ids_done
    mov rdi, qword ptr [rip + g_stmt_feed_ids]
    xor esi, esi
    call sqlite3_column_int64
    test ebx, ebx
    jnz .check_contig
    mov r14, rax
    jmp .next_feed_id
.check_contig:
    mov rcx, r14
    sub rcx, rbx
    cmp rax, rcx
    je .next_feed_id
    xor r15d, r15d
.next_feed_id:
    inc ebx
    jmp .feed_id_loop

.feed_ids_done:
    mov rdi, qword ptr [rip + g_stmt_feed_ids]
    call sqlite3_reset

    cmp ebx, 20
    jne .feed_fallback
    test r15d, r15d
    jz .feed_fallback

    # step 2: contiguous range [min_id, max_id] -> scan likes_post_id_idx range + posts PK range
    mov r13, r14
    sub r13, 19                     # r13 = min_id, r14 = max_id

    xor eax, eax
    lea rdi, [rip + g_like_counts]
    mov ecx, 24
    rep stosq

    mov rdi, qword ptr [rip + g_stmt_feed_likes]
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_feed_likes]
    mov esi, 2
    mov rdx, r14
    call sqlite3_bind_int64

    lea rbx, [rip + g_like_counts]
.feed_likes_loop:
    mov rdi, qword ptr [rip + g_stmt_feed_likes]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .feed_likes_done
    mov rdi, qword ptr [rip + g_stmt_feed_likes]
    xor esi, esi
    call sqlite3_column_int64
    sub rax, r13
    cmp rax, 20
    jae .feed_likes_loop
    inc qword ptr [rbx + rax*8]
    jmp .feed_likes_loop

.feed_likes_done:
    mov rdi, qword ptr [rip + g_stmt_feed_likes]
    call sqlite3_reset

    mov rdi, qword ptr [rip + g_stmt_feed_range]
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, qword ptr [rip + g_stmt_feed_range]
    mov esi, 2
    mov rdx, r14
    call sqlite3_bind_int64

    lea r15, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_feed_prefix]
    mov qword ptr [r15], rax
    mov word ptr [r15 + 8], 0x5b3a  # ":["
    add r15, LEN_FEED_PFX
    xor ebx, ebx

.feed_range_loop:
    mov rdi, qword ptr [rip + g_stmt_feed_range]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .feed_range_done

    test ebx, ebx
    jz 1f
    mov byte ptr [r15], ','
    inc r15
1:
    inc ebx

    mov rdi, qword ptr [rip + g_stmt_feed_range]
    xor esi, esi
    call sqlite3_column_int64
    mov rbp, rax                    # id

    mov rdi, qword ptr [rip + g_stmt_feed_range]
    mov esi, 1
    call sqlite3_column_text
    mov qword ptr [rsp], rax        # body

    mov rdi, qword ptr [rip + g_stmt_feed_range]
    mov esi, 2
    call sqlite3_column_text
    mov qword ptr [rsp + 8], rax    # created_at

    mov rdi, qword ptr [rip + g_stmt_feed_range]
    mov esi, 3
    call sqlite3_column_text
    mov r8, rax                     # author

    mov rax, rbp
    sub rax, r13
    lea rcx, [rip + g_like_counts]
    mov r9, qword ptr [rcx + rax*8] # like_count

    mov rdi, r15
    mov rsi, rbp
    mov rdx, qword ptr [rsp]
    mov rcx, qword ptr [rsp + 8]
    call append_post_json
    mov r15, rax
    jmp .feed_range_loop

.feed_range_done:
    mov rdi, qword ptr [rip + g_stmt_feed_range]
    call sqlite3_reset

    mov word ptr [r15], 0x7d5d      # "]}"
    add r15, 2
    lea rdx, [rip + g_json_buf]
    mov rcx, r15
    sub rcx, rdx
    mov edi, r12d
    mov esi, 200
    call send_http_response
    jmp .req_done

.feed_fallback:
    lea r15, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_feed_prefix]
    mov qword ptr [r15], rax
    mov word ptr [r15 + 8], 0x5b3a
    add r15, LEN_FEED_PFX
    xor ebx, ebx

.feed_slow_loop:
    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .feed_slow_done

    test ebx, ebx
    jz 1f
    mov byte ptr [r15], ','
    inc r15
1:
    inc ebx

    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    xor esi, esi
    call sqlite3_column_int64
    mov rbp, rax

    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    mov esi, 1
    call sqlite3_column_text
    mov qword ptr [rsp], rax

    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    mov esi, 2
    call sqlite3_column_text
    mov qword ptr [rsp + 8], rax

    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    mov esi, 3
    call sqlite3_column_text
    mov qword ptr [rsp + 16], rax

    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    mov esi, 4
    call sqlite3_column_int64
    mov r9, rax

    mov rdi, r15
    mov rsi, rbp
    mov rdx, qword ptr [rsp]
    mov rcx, qword ptr [rsp + 8]
    mov r8, qword ptr [rsp + 16]
    call append_post_json
    mov r15, rax
    jmp .feed_slow_loop

.feed_slow_done:
    mov rdi, qword ptr [rip + g_stmt_feed_slow]
    call sqlite3_reset
    mov word ptr [r15], 0x7d5d
    add r15, 2
    lea rdx, [rip + g_json_buf]
    mov rcx, r15
    sub rcx, rdx
    mov edi, r12d
    mov esi, 200
    call send_http_response
    jmp .req_done

.try_get_post:
    mov rax, 0x736f702f20544547     # "GET /pos"
    cmp qword ptr [r13], rax
    jne .try_health
    cmp word ptr [r13 + 8], 0x7374  # "ts"
    jne .ret_404
    cmp byte ptr [r13 + 10], '/'
    jne .ret_404

    lea rdi, [r13 + 11]
    xor esi, esi
1:
    movzx eax, byte ptr [rdi + rsi]
    cmp al, ' '
    je 2f
    cmp al, '?'
    je 2f
    cmp al, 13
    je 2f
    test al, al
    jz .ret_404
    inc rsi
    jmp 1b
2:
    call parse_positive_int
    test rax, rax
    jle .ret_400_invalid_id
    mov rbx, rax

    call ensure_read_tx
    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 1
    mov rdx, rbx
    call sqlite3_bind_int64

    mov rdi, qword ptr [rip + g_stmt_get_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .post_missing

    mov rdi, qword ptr [rip + g_stmt_get_post]
    xor esi, esi
    call sqlite3_column_int64
    mov rbp, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 1
    call sqlite3_column_text
    mov r13, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 2
    call sqlite3_column_text
    mov r14, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 3
    call sqlite3_column_text
    mov r15, rax

    mov rdi, qword ptr [rip + g_stmt_get_post]
    mov esi, 4
    call sqlite3_column_int64
    mov r9, rax

    lea rdi, [rip + g_json_buf]
    mov rax, qword ptr [rip + str_post_prefix]
    mov qword ptr [rdi], rax

    lea rdi, [rip + g_json_buf + LEN_POST_PFX]
    mov rsi, rbp
    mov rdx, r13
    mov rcx, r14
    mov r8, r15
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
    mov ecx, LEN_ERR_PNF
    call send_http_response
    jmp .req_done

.try_health:
    mov rax, 0x6165682f20544547     # "GET /hea"
    cmp qword ptr [r13], rax
    jne .ret_404
    cmp word ptr [r13 + 8], 0x746c  # "lt"
    jne .ret_404
    cmp byte ptr [r13 + 10], 'h'
    jne .ret_404
    movzx eax, byte ptr [r13 + 11]
    cmp al, ' '
    je .do_health
    cmp al, '?'
    je .do_health
    jmp .ret_404

.do_health:
    xor edi, edi
    call time
    mov qword ptr [rip + g_now_time], rax

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

    mov rax, qword ptr [rip + g_now_time]
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

.ret_404:
    mov edi, r12d
    mov esi, 404
    lea rdx, [rip + err_not_found]
    mov ecx, LEN_ERR_NF
    call send_http_response
    jmp .req_done

.ret_400_invalid_id:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_invalid_id]
    mov ecx, LEN_ERR_ID
    call send_http_response
    jmp .req_done

.ret_400_malformed:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_malformed_json]
    mov ecx, LEN_ERR_MAL
    call send_http_response
    jmp .req_done

.ret_400_required:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_required]
    mov ecx, LEN_ERR_REQ
    call send_http_response
    jmp .req_done

.ret_400_too_long:
    mov edi, r12d
    mov esi, 400
    lea rdx, [rip + err_body_too_long]
    mov ecx, LEN_ERR_LONG
    call send_http_response
    jmp .req_done

.ret_401_missing:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_missing_token]
    mov ecx, LEN_ERR_MTOK
    call send_http_response
    jmp .req_done

.ret_401_invalid:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_token]
    mov ecx, LEN_ERR_ITOK
    call send_http_response
    jmp .req_done

.ret_401_payload:
    mov edi, r12d
    mov esi, 401
    lea rdx, [rip + err_invalid_pay]
    mov ecx, LEN_ERR_IPAY
    call send_http_response
    jmp .req_done

.ret_503:
    mov edi, r12d
    mov esi, 503
    lea rdx, [rip + err_degraded]
    mov ecx, LEN_ERR_DEG
    call send_http_response
    jmp .req_done

.ret_500:
    mov edi, r12d
    mov esi, 500
    lea rdx, [rip + err_internal]
    mov ecx, LEN_ERR_INT
    call send_http_response

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
# when accepted fd reaches g_fd_limit, hand off listen_fd to the sleeping standby worker!
drain_accept:
    lea r9, [rip + g_epoll_ev]
    mov dword ptr [r9], EPOLLIN
    mov dword ptr [r9 + 8], 0
.acc_next:
    mov eax, SYS_ACCEPT4
    mov edi, r12d
    xor esi, esi
    xor edx, edx
    mov r10d, SOCK_NONBLOCK_CLOEXEC
    syscall
    test eax, eax
    js .acc_done
    mov r8d, eax
    mov dword ptr [r9 + 4], eax
    mov edx, eax
    mov eax, SYS_EPOLL_CTL
    mov edi, r13d
    mov esi, EPOLL_CTL_ADD
    mov r10, r9
    syscall

    cmp r8d, dword ptr [rip + g_fd_limit]
    jl .acc_next
    cmp qword ptr [rip + g_handed_off], 0
    jne .acc_next
    cmp qword ptr [rip + g_next_efd], 0
    jle .acc_next

    # hand off listen_fd baton: remove listen_fd from this worker's epoll and wake standby worker
    mov qword ptr [rip + g_handed_off], 1
    mov eax, SYS_EPOLL_CTL
    mov edi, r13d
    mov esi, EPOLL_CTL_DEL
    mov edx, r12d
    xor r10d, r10d
    syscall

    mov qword ptr [rip + g_num_tmp], 1
    mov eax, SYS_WRITE
    mov edi, dword ptr [rip + g_next_efd]
    lea rsi, [rip + g_num_tmp]
    mov edx, 8
    syscall
.acc_done:
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

    mov edi, 17
    mov esi, 1
    call signal

    lea rdi, [rip + g_resp_200_buf]
    lea rsi, [rip + hdr_200_ka]
    mov ecx, LEN_200_KA
    rep movsb

    mov qword ptr [rip + g_fd_limit], DEF_FD_LIMIT
    mov eax, SYS_GETRLIMIT
    mov edi, RLIMIT_NOFILE
    mov rsi, rsp
    syscall
    test eax, eax
    jnz .check_env_fd_limit
    mov rax, qword ptr [rsp]
    sub rax, 1535
    cmp rax, 1024
    jl .check_env_fd_limit
    mov qword ptr [rip + g_fd_limit], rax

.check_env_fd_limit:
    lea rdi, [rip + env_fd_limit]
    call getenv
    test rax, rax
    jz .rlim_done
    mov rdi, rax
    call atoi
    cmp eax, 16
    jl .rlim_done
    mov qword ptr [rip + g_fd_limit], rax
.rlim_done:

    xor edi, edi
    call time
    mov qword ptr [rip + g_start_time], rax
    mov qword ptr [rip + g_now_time], rax

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
    call init_hmac_ctx

    # open shared listen socket once before any standby fork so standby workers have empty fd tables
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

    lea rdi, [rip + env_sqlite_path]
    call getenv
    test rax, rax
    jnz 2f
    lea rax, [rip + def_sqlite_path]
2:
    mov qword ptr [rip + g_db_path], rax

.spawn_standby:
    # if worker_idx < MAX_WORKERS - 1, fork one sleeping standby child with a clean fd table
    mov rax, qword ptr [rip + g_worker_idx]
    cmp rax, (MAX_WORKERS - 1)
    jge .open_worker_db

    mov eax, SYS_EVENTFD2
    xor edi, edi
    xor esi, esi
    syscall
    test eax, eax
    js .open_worker_db
    mov r14d, eax                   # efd

    mov eax, SYS_FORK
    syscall
    test eax, eax
    js .open_worker_db
    jz .standby_child_wait

    # active worker saves efd in g_next_efd to wake standby child when g_fd_limit is reached
    mov qword ptr [rip + g_next_efd], r14
    jmp .open_worker_db

.standby_child_wait:
    # standby child sleeps on eventfd (0% CPU, 0 sqlite connections) until parent hands off listen_fd
    inc qword ptr [rip + g_worker_idx]
    mov qword ptr [rip + g_next_efd], 0
    mov qword ptr [rip + g_handed_off], 0
.wait_efd_read:
    mov eax, SYS_READ
    mov edi, r14d
    mov rsi, rsp
    mov edx, 8
    syscall
    cmp rax, 8
    jne .wait_efd_read

    mov eax, SYS_CLOSE
    mov edi, r14d
    syscall
    jmp .spawn_standby              # woken standby worker forks the next standby worker and starts!

.open_worker_db:
    mov rdi, qword ptr [rip + g_db_path]
    lea rsi, [rip + g_db]
    mov edx, SQLITE_OPEN_RW_NOMUTEX
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .fatal

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_pragmas_w0]
    cmp qword ptr [rip + g_worker_idx], 0
    je 5f
    lea rsi, [rip + sql_pragmas_wn]
5:
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec

    lea rsi, [rip + sql_health]
    lea r8, [rip + g_stmt_health]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_feed_ids]
    lea r8, [rip + g_stmt_feed_ids]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_feed_likes]
    lea r8, [rip + g_stmt_feed_likes]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_feed_range]
    lea r8, [rip + g_stmt_feed_range]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_feed_slow]
    lea r8, [rip + g_stmt_feed_slow]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_get_post]
    lea r8, [rip + g_stmt_get_post]
    call prep_stmt
    test eax, eax
    jnz .fatal

    lea rsi, [rip + sql_chk_like]
    lea r8, [rip + g_stmt_chk_like]
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

    mov rdi, qword ptr [rip + g_db]
    lea rsi, [rip + sql_busy_zero]
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec

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

    cmp qword ptr [rip + g_worker_idx], 0
    jne .ev_loop
    mov eax, SYS_WRITE
    mov edi, 2
    lea rsi, [rip + msg_up]
    mov edx, LEN_MSG_UP
    syscall

.ev_loop:
    mov qword ptr [rip + g_coalesced], 0
    mov r10d, -1
    cmp qword ptr [rip + g_batch_count], 0
    jle .do_epoll_wait
    mov r10d, 1                     # 1ms retry timeout if a write batch is waiting on SQLITE_BUSY
.do_epoll_wait:
    mov eax, SYS_EPOLL_WAIT
    mov edi, r13d
    lea rsi, [rip + g_epoll_events]
    mov edx, MAX_EVENTS
    syscall
    test eax, eax
    jg .got_events
    jne .ev_loop
    cmp qword ptr [rip + g_batch_count], 0
    jle .ev_loop
    call flush_group_commit
    jmp .ev_loop

.got_events:
    mov r14d, eax
    xor edi, edi
    call time
    mov qword ptr [rip + g_now_time], rax
    mov eax, r14d

.proc_batch:
    mov r14d, eax
    mov qword ptr [rip + g_tick_evs], r14
    xor r15d, r15d

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
    # always close the per-batch read snapshot before checking write queue
    cmp qword ptr [rip + g_in_tx], 0
    je .check_writes
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_step
    mov rdi, qword ptr [rip + g_stmt_commit]
    call sqlite3_reset
    mov qword ptr [rip + g_in_tx], 0

.check_writes:
    cmp qword ptr [rip + g_batch_count], 0
    jle .ev_loop

.maybe_coalesce:
    cmp qword ptr [rip + g_tick_evs], 8
    jl .do_flush
    cmp qword ptr [rip + g_coalesced], 2
    jge .do_flush
    cmp qword ptr [rip + g_batch_count], 64
    jge .do_flush
    inc qword ptr [rip + g_coalesced]
    mov eax, SYS_EPOLL_WAIT
    mov edi, r13d
    lea rsi, [rip + g_epoll_events]
    mov edx, MAX_EVENTS
    xor r10d, r10d
    syscall
    test eax, eax
    jg .proc_batch

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