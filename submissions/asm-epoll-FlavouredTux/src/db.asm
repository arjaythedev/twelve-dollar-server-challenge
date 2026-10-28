; db.asm - SQLite database interface and prepared statements in x86_64 NASM
default rel

%include "common.inc"

extern sqlite3_open_v2
extern sqlite3_exec
extern sqlite3_prepare_v2
extern sqlite3_step
extern sqlite3_reset
extern sqlite3_clear_bindings
extern sqlite3_bind_int64
extern sqlite3_bind_text
extern sqlite3_column_int64
extern sqlite3_column_text
extern sqlite3_column_bytes
extern sqlite3_changes
extern sqlite3_errmsg
extern sqlite3_close
extern sqlite3_wal_checkpoint_v2
extern u64_to_str
extern json_escape_string

section .rodata

pragmas_sql:
    db "PRAGMA journal_mode = WAL; "
    db "PRAGMA synchronous = NORMAL; "
    db "PRAGMA mmap_size = 134217728; "
    db "PRAGMA cache_size = -32000; "
    db "PRAGMA wal_autocheckpoint = 1000; "
    db "PRAGMA journal_size_limit = 67108864; "
    db "PRAGMA temp_store = MEMORY; "
    db "PRAGMA busy_timeout = 5000;", 0

sql_health:
    db "SELECT 1;", 0

sql_feed:
    db "SELECT p.id, p.body, p.created_at, u.username, "
    db "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
    db "FROM posts p JOIN users u ON u.id = p.user_id "
    db "ORDER BY p.created_at DESC, p.id DESC "
    db "LIMIT 20;", 0

sql_get_post:
    db "SELECT p.id, p.body, p.created_at, u.username, "
    db "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count "
    db "FROM posts p JOIN users u ON u.id = p.user_id "
    db "WHERE p.id = ?;", 0

sql_check_post:
    db "SELECT 1 FROM posts WHERE id = ?;", 0

sql_insert_post:
    db "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;", 0

sql_insert_like:
    db "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING;", 0

str_feed_start:
    db '{"posts":['
str_feed_start_len equ $ - str_feed_start

str_feed_end:
    db ']}'
str_feed_end_len equ $ - str_feed_end

str_post_prefix:
    db '{"id":'
str_post_prefix_len equ $ - str_post_prefix

str_post_body_key:
    db ',"body":"'
str_post_body_key_len equ $ - str_post_body_key

str_post_created_key:
    db '","created_at":"'
str_post_created_key_len equ $ - str_post_created_key

str_post_author_key:
    db '","author":"'
str_post_author_key_len equ $ - str_post_author_key

str_post_like_key:
    db '","like_count":'
str_post_like_key_len equ $ - str_post_like_key

str_post_wrap_start:
    db '{"post":'
str_post_wrap_start_len equ $ - str_post_wrap_start

str_post_wrap_end:
    db '}'
str_post_wrap_end_len equ $ - str_post_wrap_end

str_like_first:
    db '{"liked":true,"already_liked":false,"post_id":'
str_like_first_len equ $ - str_like_first

str_like_repeat:
    db '{"liked":true,"already_liked":true,"post_id":'
str_like_repeat_len equ $ - str_like_repeat

section .bss
global db_handle
db_handle:          resq 1
stmt_health:        resq 1
stmt_feed:          resq 1
stmt_get_post:      resq 1
stmt_check_post:    resq 1
stmt_insert_post:   resq 1
stmt_insert_like:   resq 1
write_counter:      resd 1

section .text

global init_db
global db_check_health
global db_get_feed
global db_get_post
global db_create_post
global db_like_post

; Checkpoint WAL periodically to keep WAL file size bounded
maybe_checkpoint:
    inc dword [write_counter]
    test dword [write_counter], 0x1ff   ; every 512 writes
    jnz .done
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    mov rdi, [db_handle]
    xor esi, esi            ; NULL = all databases / main
    mov edx, 2              ; SQLITE_CHECKPOINT_RESTART (2) or SQLITE_CHECKPOINT_PASSIVE (0)
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_wal_checkpoint_v2
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
.done:
    ret

; =============================================================================
; init_db(const char *path [rdi]) -> rax (0 = success, 1 = failure)
; =============================================================================
init_db:
    push rbx
    push r12
    push rbp
    mov rbp, rsp
    sub rsp, 16

    mov rsi, rdi            ; path
    mov rdi, rsi
    lea rsi, [db_handle]
    mov edx, SQLITE_OPEN_READWRITE
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .err

    ; Run pragmas
    mov rdi, [db_handle]
    lea rsi, [pragmas_sql]
    xor edx, edx
    xor ecx, ecx
    lea r8, [rsp]
    call sqlite3_exec
    test eax, eax
    jnz .err

    ; Prepare stmt_health
    mov rdi, [db_handle]
    lea rsi, [sql_health]
    mov edx, -1
    lea rcx, [stmt_health]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    ; Prepare stmt_feed
    mov rdi, [db_handle]
    lea rsi, [sql_feed]
    mov edx, -1
    lea rcx, [stmt_feed]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    ; Prepare stmt_get_post
    mov rdi, [db_handle]
    lea rsi, [sql_get_post]
    mov edx, -1
    lea rcx, [stmt_get_post]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    ; Prepare stmt_check_post
    mov rdi, [db_handle]
    lea rsi, [sql_check_post]
    mov edx, -1
    lea rcx, [stmt_check_post]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    ; Prepare stmt_insert_post
    mov rdi, [db_handle]
    lea rsi, [sql_insert_post]
    mov edx, -1
    lea rcx, [stmt_insert_post]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    ; Prepare stmt_insert_like
    mov rdi, [db_handle]
    lea rsi, [sql_insert_like]
    mov edx, -1
    lea rcx, [stmt_insert_like]
    xor r8, r8
    call sqlite3_prepare_v2
    test eax, eax
    jnz .err

    xor eax, eax            ; success!
    leave
    pop r12
    pop rbx
    ret

.err:
    mov eax, 1
    leave
    pop r12
    pop rbx
    ret

; =============================================================================
; db_check_health(void) -> rax (1 = ok, 0 = fail)
; =============================================================================
db_check_health:
    push rbp
    mov rbp, rsp
    mov rdi, [stmt_health]
    test rdi, rdi
    jz .fail
    call sqlite3_reset
    mov rdi, [stmt_health]
    call sqlite3_step
    push rax
    mov rdi, [stmt_health]
    call sqlite3_reset
    pop rax
    cmp eax, SQLITE_ROW
    jne .fail
    mov eax, 1
    leave
    ret
.fail:
    xor eax, eax
    leave
    ret

; =============================================================================
; Helper: append bytes to buffer
; rdi = dst, rsi = src, rdx = len. Returns rdi advanced by len.
; =============================================================================
append_bytes:
    push rcx
    mov rcx, rdx
    cld
    rep movsb
    pop rcx
    ret

; =============================================================================
; Helper: format single post object into dst buffer
; rdi = dst buffer
; rsi = sqlite3_stmt
; Returns: rdi advanced to end of formatted post object
; =============================================================================
format_post_object:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp
    sub rsp, 40

    mov r12, rdi            ; dst
    mov r13, rsi            ; stmt

    ; 1. '{"id":'
    mov rdi, r12
    lea rsi, [str_post_prefix]
    mov rdx, str_post_prefix_len
    call append_bytes
    mov r12, rdi

    ; 2. post id
    mov rdi, r13
    xor esi, esi            ; column 0 = id
    call sqlite3_column_int64
    mov rdi, rax
    mov rsi, r12
    call u64_to_str
    add r12, rax

    ; 3. ',"body":"'
    mov rdi, r12
    lea rsi, [str_post_body_key]
    mov rdx, str_post_body_key_len
    call append_bytes
    mov r12, rdi

    ; 4. escaped body
    mov rdi, r13
    mov esi, 1              ; column 1 = body
    call sqlite3_column_text
    mov r14, rax            ; body text ptr

    mov rdi, r13
    mov esi, 1
    call sqlite3_column_bytes
    mov r15, rax            ; body byte len

    mov rdi, r14
    mov rsi, r15
    mov rdx, r12
    call json_escape_string
    add r12, rax

    ; 5. '","created_at":"'
    mov rdi, r12
    lea rsi, [str_post_created_key]
    mov rdx, str_post_created_key_len
    call append_bytes
    mov r12, rdi

    ; 6. created_at
    mov rdi, r13
    mov esi, 2              ; column 2 = created_at
    call sqlite3_column_text
    mov r14, rax

    mov rdi, r13
    mov esi, 2
    call sqlite3_column_bytes
    mov rdx, rax
    mov rdi, r12
    mov rsi, r14
    call append_bytes
    mov r12, rdi

    ; 7. '","author":"'
    mov rdi, r12
    lea rsi, [str_post_author_key]
    mov rdx, str_post_author_key_len
    call append_bytes
    mov r12, rdi

    ; 8. author
    mov rdi, r13
    mov esi, 3              ; column 3 = username
    call sqlite3_column_text
    mov r14, rax

    mov rdi, r13
    mov esi, 3
    call sqlite3_column_bytes
    mov rdx, rax
    mov rdi, r12
    mov rsi, r14
    call append_bytes
    mov r12, rdi

    ; 9. '","like_count":'
    mov rdi, r12
    lea rsi, [str_post_like_key]
    mov rdx, str_post_like_key_len
    call append_bytes
    mov r12, rdi

    ; 10. like_count
    mov rdi, r13
    mov esi, 4              ; column 4 = like_count
    call sqlite3_column_int64
    mov rdi, rax
    mov rsi, r12
    call u64_to_str
    add r12, rax

    ; 11. '}'
    mov byte [r12], '}'
    inc r12

    mov rax, r12            ; return advanced dst pointer
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; db_get_feed(char *resp_body [rdi], size_t *out_len [rsi])
; =============================================================================
db_get_feed:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    mov rbp, rsp

    mov r12, rdi            ; resp_body
    mov r13, rsi            ; out_len ptr
    mov r14, r12            ; save start

    ; Reset stmt_feed
    mov rdi, [stmt_feed]
    call sqlite3_reset

    ; Append '{"posts":['
    mov rdi, r12
    lea rsi, [str_feed_start]
    mov rdx, str_feed_start_len
    call append_bytes
    mov r12, rdi

    xor ebx, ebx            ; row count

.step_loop:
    mov rdi, [stmt_feed]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .step_done

    ; If ebx > 0, append ','
    test ebx, ebx
    jz .format_row
    mov byte [r12], ','
    inc r12

.format_row:
    mov rdi, r12
    mov rsi, [stmt_feed]
    call format_post_object
    mov r12, rax            ; updated dst

    inc ebx
    jmp .step_loop

.step_done:
    ; Append ']}'
    mov rdi, r12
    lea rsi, [str_feed_end]
    mov rdx, str_feed_end_len
    call append_bytes
    mov r12, rdi

    ; Calculate length
    mov rax, r12
    sub rax, r14
    mov [r13], rax
    mov byte [r12], 0       ; null terminate

    ; Reset stmt_feed to release SQLite read transaction
    mov rdi, [stmt_feed]
    call sqlite3_reset

    xor eax, eax
    leave
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; db_get_post(uint64_t post_id [rdi], char *resp_body [rsi], size_t *out_len [rdx])
; Returns: 0 = found (200), 1 = not found (404)
; =============================================================================
db_get_post:
    push rbx
    push r12
    push r13
    push r14
    push rbp
    mov rbp, rsp

    mov rbx, rdi            ; post_id
    mov r12, rsi            ; resp_body
    mov r13, rdx            ; out_len
    mov r14, r12

    ; Reset stmt_get_post
    mov rdi, [stmt_get_post]
    call sqlite3_reset

    ; Bind post_id to parameter 1
    mov rdi, [stmt_get_post]
    mov esi, 1
    mov rdx, rbx
    call sqlite3_bind_int64

    ; Step
    mov rdi, [stmt_get_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .not_found

    ; Append '{"post":'
    mov rdi, r12
    lea rsi, [str_post_wrap_start]
    mov rdx, str_post_wrap_start_len
    call append_bytes
    mov r12, rdi

    ; Format post object
    mov rdi, r12
    mov rsi, [stmt_get_post]
    call format_post_object
    mov r12, rax

    ; Append '}'
    mov byte [r12], '}'
    inc r12

    ; Length
    mov rax, r12
    sub rax, r14
    mov [r13], rax
    mov byte [r12], 0

    ; Reset stmt_get_post to release SQLite read transaction
    mov rdi, [stmt_get_post]
    call sqlite3_reset

    xor eax, eax            ; found!
    leave
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.not_found:
    mov rdi, [stmt_get_post]
    call sqlite3_reset
    mov eax, 1
    leave
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; db_create_post(uint64_t user_id [rdi], const char *username [rsi],
;                const char *body [rdx], size_t body_len [rcx],
;                char *resp_body [r8], size_t *out_len [r9])
; Returns: 0 = success (201), 1 = error (500)
; =============================================================================
db_create_post:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp
    sub rsp, 56

    mov [rbp - 8], rdi      ; user_id
    mov [rbp - 16], rsi     ; username
    mov [rbp - 24], rdx     ; body
    mov [rbp - 32], rcx     ; body_len
    mov r12, r8             ; resp_body
    mov r13, r9             ; out_len ptr
    mov r14, r12

    ; Reset stmt_insert_post
    mov rdi, [stmt_insert_post]
    call sqlite3_reset

    ; Bind user_id
    mov rdi, [stmt_insert_post]
    mov esi, 1
    mov rdx, [rbp - 8]
    call sqlite3_bind_int64

    ; Bind body
    mov rdi, [stmt_insert_post]
    mov esi, 2
    mov rdx, [rbp - 24]     ; body ptr
    mov rcx, [rbp - 32]     ; body len
    xor r8, r8              ; SQLITE_STATIC (0)
    call sqlite3_bind_text

    ; Step: RETURNING id, created_at
    mov rdi, [stmt_insert_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .err

    ; Get returned id
    mov rdi, [stmt_insert_post]
    xor esi, esi
    call sqlite3_column_int64
    mov [rbp - 40], rax     ; new post id

    ; Format output: '{"post":{"id":<id>,"body":"<body_escaped>","created_at":"<created_at>","author":"<username>","like_count":0}}'
    mov rdi, r12
    lea rsi, [str_post_wrap_start]
    mov rdx, str_post_wrap_start_len
    call append_bytes
    mov r12, rdi

    ; '{"id":'
    mov rdi, r12
    lea rsi, [str_post_prefix]
    mov rdx, str_post_prefix_len
    call append_bytes
    mov r12, rdi

    ; new id
    mov rdi, [rbp - 40]
    mov rsi, r12
    call u64_to_str
    add r12, rax

    ; ',"body":"'
    mov rdi, r12
    lea rsi, [str_post_body_key]
    mov rdx, str_post_body_key_len
    call append_bytes
    mov r12, rdi

    ; escaped body
    mov rdi, [rbp - 24]
    mov rsi, [rbp - 32]
    mov rdx, r12
    call json_escape_string
    add r12, rax

    ; '","created_at":"'
    mov rdi, r12
    lea rsi, [str_post_created_key]
    mov rdx, str_post_created_key_len
    call append_bytes
    mov r12, rdi

    ; created_at from column 1
    mov rdi, [stmt_insert_post]
    mov esi, 1
    call sqlite3_column_text
    mov r15, rax

    mov rdi, [stmt_insert_post]
    mov esi, 1
    call sqlite3_column_bytes
    mov rdx, rax
    mov rdi, r12
    mov rsi, r15
    call append_bytes
    mov r12, rdi

    ; '","author":"'
    mov rdi, r12
    lea rsi, [str_post_author_key]
    mov rdx, str_post_author_key_len
    call append_bytes
    mov r12, rdi

    ; username string
    mov rdi, [rbp - 16]
    xor ecx, ecx
.len_u:
    cmp byte [rdi + rcx], 0
    je .done_u
    inc rcx
    jmp .len_u
.done_u:
    mov rdx, rcx
    mov rdi, r12
    mov rsi, [rbp - 16]
    call append_bytes
    mov r12, rdi

    ; '","like_count":0}}'
    mov rdi, r12
    lea rsi, [str_post_like_key]
    mov rdx, str_post_like_key_len
    call append_bytes
    mov r12, rdi

    mov byte [r12], '0'
    mov byte [r12 + 1], '}'
    mov byte [r12 + 2], '}'
    add r12, 3

    ; Length
    mov rax, r12
    sub rax, r14
    mov [r13], rax
    mov byte [r12], 0

    ; Reset stmt and checkpoint
    mov rdi, [stmt_insert_post]
    call sqlite3_reset
    call maybe_checkpoint

    xor eax, eax            ; success
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.err:
    mov rdi, [stmt_insert_post]
    call sqlite3_reset
    mov eax, 1
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; db_like_post(uint64_t user_id [rdi], uint64_t post_id [rsi],
;              char *resp_body [rdx], size_t *out_len [rcx])
; Returns:
;   201 = first like (201 Created)
;   200 = repeat like (200 OK)
;   404 = post not found (404 Not Found)
; =============================================================================
db_like_post:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp
    sub rsp, 40

    mov [rbp - 8], rdi      ; user_id
    mov [rbp - 16], rsi     ; post_id
    mov r12, rdx            ; resp_body
    mov r13, rcx            ; out_len
    mov r14, r12

    ; 1. Check if post exists!
    mov rdi, [stmt_check_post]
    call sqlite3_reset

    mov rdi, [stmt_check_post]
    mov esi, 1
    mov rdx, [rbp - 16]     ; post_id
    call sqlite3_bind_int64

    mov rdi, [stmt_check_post]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .post_not_found

    ; Reset stmt_check_post immediately so read lock is released
    mov rdi, [stmt_check_post]
    call sqlite3_reset

    ; 2. Post exists! Insert like
    mov rdi, [stmt_insert_like]
    call sqlite3_reset

    mov rdi, [stmt_insert_like]
    mov esi, 1
    mov rdx, [rbp - 8]      ; user_id
    call sqlite3_bind_int64

    mov rdi, [stmt_insert_like]
    mov esi, 2
    mov rdx, [rbp - 16]     ; post_id
    call sqlite3_bind_int64

    mov rdi, [stmt_insert_like]
    call sqlite3_step

    ; Check sqlite3_changes
    mov rdi, [db_handle]
    call sqlite3_changes
    test eax, eax
    jz .already_liked

    ; First like: 201 Created
    ; '{"liked":true,"already_liked":false,"post_id":'
    mov rdi, r12
    lea rsi, [str_like_first]
    mov rdx, str_like_first_len
    call append_bytes
    mov r12, rdi

    ; post_id
    mov rdi, [rbp - 16]
    mov rsi, r12
    call u64_to_str
    add r12, rax

    ; '}'
    mov byte [r12], '}'
    inc r12

    ; Length
    mov rax, r12
    sub rax, r14
    mov [r13], rax
    mov byte [r12], 0

    ; Reset insert_like
    mov rdi, [stmt_insert_like]
    call sqlite3_reset
    call maybe_checkpoint

    mov eax, 201
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.already_liked:
    ; Repeat like: 200 OK
    ; '{"liked":true,"already_liked":true,"post_id":'
    mov rdi, r12
    lea rsi, [str_like_repeat]
    mov rdx, str_like_repeat_len
    call append_bytes
    mov r12, rdi

    ; post_id
    mov rdi, [rbp - 16]
    mov rsi, r12
    call u64_to_str
    add r12, rax

    ; '}'
    mov byte [r12], '}'
    inc r12

    ; Length
    mov rax, r12
    sub rax, r14
    mov [r13], rax
    mov byte [r12], 0

    ; Reset insert_like
    mov rdi, [stmt_insert_like]
    call sqlite3_reset
    call maybe_checkpoint

    mov eax, 200
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.post_not_found:
    mov rdi, [stmt_check_post]
    call sqlite3_reset
    mov eax, 404
    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret
