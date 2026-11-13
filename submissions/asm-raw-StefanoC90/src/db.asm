; SQLite layer.  One connection per worker thread, WAL + synchronous=NORMAL,
; with the five statements this API needs prepared once per connection.

%include "common.inc"

extern sqlite3_open_v2
extern sqlite3_close
extern sqlite3_busy_timeout
extern sqlite3_exec
extern sqlite3_prepare_v2
extern sqlite3_bind_int64
extern sqlite3_bind_text
extern sqlite3_step
extern sqlite3_reset
extern sqlite3_column_int64
extern sqlite3_column_text
extern sqlite3_column_bytes
extern sqlite3_changes
extern sqlite3_finalize

extern append_bytes
extern append_u64
extern append_json_string

section .rodata
sql_pragmas:
    db 'PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;'
    db 'PRAGMA busy_timeout=5000;PRAGMA cache_size=-1024;PRAGMA mmap_size=67108864;', 0
sql_feed:
    db 'SELECT p.id,p.body,p.created_at,u.username,'
    db '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
    db 'FROM posts p JOIN users u ON u.id=p.user_id '
    db 'ORDER BY p.created_at DESC,p.id DESC LIMIT 20', 0
sql_get:
    db 'SELECT p.id,p.body,p.created_at,u.username,'
    db '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
    db 'FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?', 0
sql_insert_post:
    db 'INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at', 0
sql_exists:
    db 'SELECT 1 FROM posts WHERE id=?', 0
sql_insert_like:
    db 'INSERT INTO likes(user_id,post_id) VALUES(?,?) '
    db 'ON CONFLICT(user_id,post_id) DO NOTHING', 0
sql_health:
    db 'SELECT 1', 0

s_open_brace:   db '{'
s_close_brace:  db '}'
s_posts_open:   db '{"posts":['
s_posts_close:  db ']}'
s_post_open:    db '{"post":'
s_id:           db '{"id":'
s_body:         db ',"body":'
s_created:      db ',"created_at":'
s_author:       db ',"author":'
s_likes:        db ',"like_count":'

section .bss
global g_sqlite_path
g_sqlite_path: resq 1

section .text

global db_init
global db_close
global db_health
global db_feed
global db_get_post
global db_create_post
global db_like

; -----------------------------------------------------------------------------
; db_init(rdi=wctx) -> eax = 0 ok / -1
; -----------------------------------------------------------------------------
db_init:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov rdi, [rel g_sqlite_path]
    lea rsi, [r12+W_DB]
    mov edx, SQLITE_OPEN_RW
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .fail
    mov rdi, [r12+W_DB]
    mov esi, 5000
    call sqlite3_busy_timeout
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_pragmas]
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec
    test eax, eax
    jnz .fail_close
    ; feed
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_feed]
    mov edx, -1
    lea rcx, [r12+W_STMT_FEED]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; get
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_get]
    mov edx, -1
    lea rcx, [r12+W_STMT_GET]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; insert post
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_insert_post]
    mov edx, -1
    lea rcx, [r12+W_STMT_INS]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; exists
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_exists]
    mov edx, -1
    lea rcx, [r12+W_STMT_EXISTS]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; like
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_insert_like]
    mov edx, -1
    lea rcx, [r12+W_STMT_LIKE]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; health
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_health]
    mov edx, -1
    lea rcx, [r12+W_STMT_HEALTH]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    xor eax, eax
    jmp .done
.fail_close:
    mov rdi, [r12+W_DB]
    call sqlite3_close
.fail:
    mov eax, -1
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_close(rdi=wctx)
; -----------------------------------------------------------------------------
db_close:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, W_STMT_FEED
.fin_loop:
    cmp r13, W_STMT_HEALTH
    ja .close_db
    mov rdi, [r12+r13]
    test rdi, rdi
    jz .fin_next
    call sqlite3_finalize
.fin_next:
    add r13, 8
    jmp .fin_loop
.close_db:
    mov rdi, [r12+W_DB]
    test rdi, rdi
    jz .done
    call sqlite3_close
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_health(rdi=wctx) -> eax 0 ok / -1
; -----------------------------------------------------------------------------
db_health:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    mov [rbp-8], rdi
    mov rsi, [rdi+W_STMT_HEALTH]
    mov [rbp-16], rsi
    mov rdi, rsi
    call sqlite3_reset
    mov rdi, [rbp-16]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .ok
    cmp eax, SQLITE_DONE
    je .ok
    mov rdi, [rbp-16]
    call sqlite3_reset
    mov eax, -1
    leave
    ret
.ok:
    mov rdi, [rbp-16]
    call sqlite3_reset
    xor eax, eax
    leave
    ret

; -----------------------------------------------------------------------------
; fill_row(rdi=stmt, rsi=row)
; -----------------------------------------------------------------------------
fill_row:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    mov rdi, r12
    xor esi, esi
    call sqlite3_column_int64
    mov [r13+R_ID], rax
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_text
    mov [r13+R_BODY], rax
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_BODYLEN], rax
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_text
    mov [r13+R_CREATED], rax
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_CREATEDLEN], rax
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_text
    mov [r13+R_AUTHOR], rax
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_AUTHORLEN], rax
    mov rdi, r12
    mov esi, 4
    call sqlite3_column_int64
    mov [r13+R_LIKES], rax
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; render_post(rdi=dst, rsi=row) -> rax = new end
; -----------------------------------------------------------------------------
render_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    lea rsi, [rel s_id]
    mov edx, 6
    mov rdi, r12
    call append_bytes
    mov rdi, rax
    mov rsi, [r13+R_ID]
    call append_u64
    mov r12, rax
    lea rsi, [rel s_body]
    mov edx, 8
    mov rdi, r12
    call append_bytes
    mov rdi, rax
    mov rsi, [r13+R_BODY]
    mov rdx, [r13+R_BODYLEN]
    call append_json_string
    mov r12, rax
    lea rsi, [rel s_created]
    mov edx, 14
    mov rdi, r12
    call append_bytes
    mov rdi, rax
    mov rsi, [r13+R_CREATED]
    mov rdx, [r13+R_CREATEDLEN]
    call append_json_string
    mov r12, rax
    lea rsi, [rel s_author]
    mov edx, 10
    mov rdi, r12
    call append_bytes
    mov rdi, rax
    mov rsi, [r13+R_AUTHOR]
    mov rdx, [r13+R_AUTHORLEN]
    call append_json_string
    mov r12, rax
    lea rsi, [rel s_likes]
    mov edx, 14
    mov rdi, r12
    call append_bytes
    mov rdi, rax
    mov rsi, [r13+R_LIKES]
    call append_u64
    mov r12, rax
    mov byte [r12], '}'
    inc r12
    mov rax, r12
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_feed(rdi=wctx, rsi=dst, rdx=cap) -> rax = body length / -1
; -----------------------------------------------------------------------------
db_feed:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    mov r12, rdi
    mov [rbp-152], rsi          ; dst
    mov [rbp-160], rdx          ; cap
    mov r13, rsi                ; p
    mov r14, [r12+W_STMT_FEED]
    mov rdi, r14
    call sqlite3_reset
    mov rdi, r13
    lea rsi, [rel s_posts_open]
    mov edx, 10
    call append_bytes
    mov r13, rax
    mov dword [rbp-136], 0      ; count
.loop:
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .row
    cmp eax, SQLITE_DONE
    je .done_rows
    mov rdi, r14
    call sqlite3_reset
    mov rax, -1
    jmp .out
.row:
    cmp dword [rbp-136], 0
    je .no_comma
    mov byte [r13], ','
    inc r13
.no_comma:
    mov rdi, r14
    lea rsi, [rbp-128]
    call fill_row
    mov rdi, r13
    lea rsi, [rbp-128]
    call render_post
    mov r13, rax
    inc dword [rbp-136]
    jmp .loop
.done_rows:
    mov byte [r13], ']'
    mov byte [r13+1], '}'
    add r13, 2
    mov rdi, r14
    call sqlite3_reset
    mov rax, r13
    sub rax, [rbp-152]
    jmp .out
.out:
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_get_post(rdi=wctx, rsi=id, rdx=dst, rcx=cap)
;   -> eax = 0 found / 1 not found / -1 error
; -----------------------------------------------------------------------------
db_get_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    mov r12, rdi
    mov r13, rsi                ; id
    mov [rbp-152], rdx          ; dst
    mov [rbp-160], rcx          ; cap
    mov r14, [r12+W_STMT_GET]
    mov rdi, r14
    call sqlite3_reset
    mov rdi, r14
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .row
    cmp eax, SQLITE_DONE
    je .not_found
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
    jmp .out
.row:
    mov rdi, r14
    lea rsi, [rbp-128]
    call fill_row
    mov rdi, [rbp-152]
    lea rsi, [rel s_post_open]
    mov edx, 8
    call append_bytes
    mov rdi, rax
    lea rsi, [rbp-128]
    call render_post
    mov byte [rax], '}'
    inc rax
    mov r15, rax
    sub r15, [rbp-152]
    mov rdi, r14
    call sqlite3_reset
    mov rdx, r15
    xor eax, eax
    jmp .out
.not_found:
    mov rdi, r14
    call sqlite3_reset
    mov eax, 1
.out:
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_create_post(rdi=wctx, rsi=args) -> eax 0 ok / -1
;   args: C_UID, C_BODY, C_BODYLEN, C_UNAME, C_UNAMELEN, C_DST, C_OUTLEN
; -----------------------------------------------------------------------------
db_create_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    mov r12, rdi
    mov r13, rsi
    mov r14, [r12+W_STMT_INS]
    mov rdi, r14
    call sqlite3_reset
    mov rdi, r14
    mov esi, 1
    mov rdx, [r13+C_UID]
    call sqlite3_bind_int64
    mov rdi, r14
    mov esi, 2
    mov rdx, [r13+C_BODY]
    mov ecx, [r13+C_BODYLEN]
    mov r8, SQLITE_TRANSIENT
    call sqlite3_bind_text
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .fail
    ; row: id, created_at
    mov rdi, r14
    xor esi, esi
    call sqlite3_column_int64
    mov [rbp-128+R_ID], rax
    mov rdi, r14
    mov esi, 1
    call sqlite3_column_text
    mov [rbp-128+R_CREATED], rax
    mov rdi, r14
    mov esi, 1
    call sqlite3_column_bytes
    mov eax, eax
    mov [rbp-128+R_CREATEDLEN], rax
    mov rax, [r13+C_BODY]
    mov [rbp-128+R_BODY], rax
    mov rax, [r13+C_BODYLEN]
    mov [rbp-128+R_BODYLEN], rax
    mov rax, [r13+C_UNAME]
    mov [rbp-128+R_AUTHOR], rax
    mov rax, [r13+C_UNAMELEN]
    mov [rbp-128+R_AUTHORLEN], rax
    mov qword [rbp-128+R_LIKES], 0
    ; render while created_at is still valid
    mov rdi, [r13+C_DST]
    lea rsi, [rel s_post_open]
    mov edx, 8
    call append_bytes
    mov rdi, rax
    lea rsi, [rbp-128]
    call render_post
    mov byte [rax], '}'
    inc rax
    sub rax, [r13+C_DST]
    mov [r13+C_OUTLEN], rax
    ; step to DONE so the implicit transaction is committed before we answer
.commit:
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_DONE
    je .committed
    cmp eax, SQLITE_ROW
    je .commit
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
    jmp .out
.committed:
    mov rdi, r14
    call sqlite3_reset
    xor eax, eax
    jmp .out
.fail:
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
.out:
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_like(rdi=wctx, rsi=user_id, rdx=post_id)
;   -> eax = 0 inserted / 1 already liked / 2 post not found / -1 error
; -----------------------------------------------------------------------------
db_like:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    mov r15, [r12+W_STMT_EXISTS]
    mov rdi, r15
    call sqlite3_reset
    mov rdi, r15
    mov esi, 1
    mov rdx, r14
    call sqlite3_bind_int64
    mov rdi, r15
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .exists
    cmp eax, SQLITE_DONE
    je .no_post
    mov rdi, r15
    call sqlite3_reset
    mov eax, -1
    jmp .out
.exists:
    mov rdi, r15
    call sqlite3_reset
    mov r15, [r12+W_STMT_LIKE]
    mov rdi, r15
    call sqlite3_reset
    mov rdi, r15
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, r15
    mov esi, 2
    mov rdx, r14
    call sqlite3_bind_int64
.insert:
    mov rdi, r15
    call sqlite3_step
    cmp eax, SQLITE_DONE
    je .inserted
    cmp eax, SQLITE_ROW
    je .insert
    mov rdi, r15
    call sqlite3_reset
    mov eax, -1
    jmp .out
.inserted:
    mov rdi, [r12+W_DB]
    call sqlite3_changes
    mov ebx, eax
    mov rdi, r15
    call sqlite3_reset
    test ebx, ebx
    jz .already
    xor eax, eax
    jmp .out
.already:
    mov eax, 1
    jmp .out
.no_post:
    mov rdi, r15
    call sqlite3_reset
    mov eax, 2
.out:
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

extern sqlite3_finalize
