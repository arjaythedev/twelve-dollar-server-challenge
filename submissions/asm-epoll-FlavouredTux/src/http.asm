; http.asm - High-performance HTTP/1.1 parser, router, and response builder in x86_64 NASM
default rel

%include "common.inc"

extern clock_gettime
extern u64_to_str
extern str_to_u64
extern json_parse_body
extern jwt_verify
extern db_check_health
extern db_get_feed
extern db_get_post
extern db_create_post
extern db_like_post
extern active_conns

section .rodata

; HTTP Status lines
http_200:           db "HTTP/1.1 200 OK", 13, 10
http_200_len        equ $ - http_200
http_201:           db "HTTP/1.1 201 Created", 13, 10
http_201_len        equ $ - http_201
http_400:           db "HTTP/1.1 400 Bad Request", 13, 10
http_400_len        equ $ - http_400
http_401:           db "HTTP/1.1 401 Unauthorized", 13, 10
http_401_len        equ $ - http_401
http_404:           db "HTTP/1.1 404 Not Found", 13, 10
http_404_len        equ $ - http_404
http_500:           db "HTTP/1.1 500 Internal Server Error", 13, 10
http_500_len        equ $ - http_500
http_503:           db "HTTP/1.1 503 Service Unavailable", 13, 10
http_503_len        equ $ - http_503

; Common headers
hdr_ctype:          db "Content-Type: application/json", 13, 10
hdr_ctype_len       equ $ - hdr_ctype
hdr_clen_prefix:    db "Content-Length: "
hdr_clen_prefix_len equ $ - hdr_clen_prefix
hdr_conn_keep:      db "Connection: keep-alive", 13, 10, 13, 10
hdr_conn_keep_len   equ $ - hdr_conn_keep
hdr_conn_close:     db "Connection: close", 13, 10, 13, 10
hdr_conn_close_len  equ $ - hdr_conn_close
crlf:               db 13, 10

; Error bodies
err_not_found:              db '{"error":"not found"}'
err_not_found_len           equ $ - err_not_found

err_invalid_post_id:        db '{"error":"invalid post id"}'
err_invalid_post_id_len     equ $ - err_invalid_post_id

err_post_not_found:         db '{"error":"post not found"}'
err_post_not_found_len      equ $ - err_post_not_found

err_missing_token:          db '{"error":"missing bearer token"}'
err_missing_token_len       equ $ - err_missing_token

err_invalid_token:          db '{"error":"invalid or expired token"}'
err_invalid_token_len       equ $ - err_invalid_token

err_invalid_payload:        db '{"error":"invalid token payload"}'
err_invalid_payload_len     equ $ - err_invalid_payload

err_malformed_json:         db '{"error":"malformed JSON body"}'
err_malformed_json_len      equ $ - err_malformed_json

err_body_required:          db '{"error":"body is required"}'
err_body_required_len       equ $ - err_body_required

err_body_too_long:          db '{"error":"body must be at most 500 characters"}'
err_body_too_long_len       equ $ - err_body_too_long

err_internal:               db '{"error":"internal server error"}'
err_internal_len            equ $ - err_internal

err_health_degraded:        db '{"status":"degraded","db":"unreachable","error":"unreachable"}'
err_health_degraded_len     equ $ - err_health_degraded

; Health success template
health_prefix:              db '{"status":"ok","db":"ok","uptime_s":'
health_prefix_len           equ $ - health_prefix

section .data
global server_start_time
server_start_time:  dq 0

section .bss
global jwt_secret_buf
global jwt_secret_len
global scratch_body_buf
global scratch_body_len
jwt_secret_buf:     resb 256
jwt_secret_len:     resq 1
scratch_body_buf:   resb RESP_BUF_SIZE
scratch_body_len:   resq 1

section .text

global handle_http_request

; =============================================================================
; Helper: append bytes to destination
; =============================================================================
append:
    push rcx
    mov rcx, rdx
    cld
    rep movsb
    pop rcx
    ret

; =============================================================================
; build_response(int status_code [rdi], const char *body [rsi], size_t body_len [rdx],
;                int keep_alive [rcx], char *resp_buf [r8], size_t *out_resp_len [r9])
; =============================================================================
build_response:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp
    sub rsp, 40

    mov r12d, edi           ; status code
    mov r13, rsi            ; body ptr
    mov r14, rdx            ; body len
    mov r15d, ecx           ; keep_alive
    mov rbx, r8             ; dst
    mov [rbp - 8], r8       ; save start
    mov [rbp - 16], r9      ; out_resp_len

    ; 1. Status line
    cmp r12d, 200
    je .st_200
    cmp r12d, 201
    je .st_201
    cmp r12d, 400
    je .st_400
    cmp r12d, 401
    je .st_401
    cmp r12d, 404
    je .st_404
    cmp r12d, 503
    je .st_503
    jmp .st_500

.st_200:
    lea rsi, [http_200]
    mov rdx, http_200_len
    jmp .put_status
.st_201:
    lea rsi, [http_201]
    mov rdx, http_201_len
    jmp .put_status
.st_400:
    lea rsi, [http_400]
    mov rdx, http_400_len
    jmp .put_status
.st_401:
    lea rsi, [http_401]
    mov rdx, http_401_len
    jmp .put_status
.st_404:
    lea rsi, [http_404]
    mov rdx, http_404_len
    jmp .put_status
.st_503:
    lea rsi, [http_503]
    mov rdx, http_503_len
    jmp .put_status
.st_500:
    lea rsi, [http_500]
    mov rdx, http_500_len

.put_status:
    mov rdi, rbx
    call append
    mov rbx, rdi

    ; 2. Content-Type header
    mov rdi, rbx
    lea rsi, [hdr_ctype]
    mov rdx, hdr_ctype_len
    call append
    mov rbx, rdi

    ; 3. Content-Length header
    mov rdi, rbx
    lea rsi, [hdr_clen_prefix]
    mov rdx, hdr_clen_prefix_len
    call append
    mov rbx, rdi

    mov rdi, r14            ; body_len
    mov rsi, rbx
    call u64_to_str
    add rbx, rax

    mov rdi, rbx
    lea rsi, [crlf]
    mov rdx, 2
    call append
    mov rbx, rdi

    ; 4. Connection header
    mov rdi, rbx
    test r15d, r15d
    jz .conn_close
    lea rsi, [hdr_conn_keep]
    mov rdx, hdr_conn_keep_len
    jmp .put_conn
.conn_close:
    lea rsi, [hdr_conn_close]
    mov rdx, hdr_conn_close_len
.put_conn:
    call append
    mov rbx, rdi

    ; 5. Body
    test r14, r14
    jz .done_body
    mov rdi, rbx
    mov rsi, r13
    mov rdx, r14
    call append
    mov rbx, rdi

.done_body:
    ; Calculate total response length
    mov rax, rbx
    sub rax, [rbp - 8]
    mov r9, [rbp - 16]
    mov [r9], rax

    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; handle_http_request(const char *req [rdi], size_t req_len [rsi],
;                     char *resp_buf [rdx], size_t *out_resp_len [rcx],
;                     int *out_keep_alive [r8])
; =============================================================================
handle_http_request:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rbp
    mov rbp, rsp

    ; Local frame:
    ; [rbp - 8]:    req_buf
    ; [rbp - 16]:   req_len
    ; [rbp - 24]:   resp_buf
    ; [rbp - 32]:   out_resp_len ptr
    ; [rbp - 40]:   out_keep_alive ptr
    ; [rbp - 48]:   method (1=GET, 2=POST)
    ; [rbp - 56]:   path_ptr
    ; [rbp - 64]:   path_len
    ; [rbp - 72]:   bearer_token_ptr
    ; [rbp - 80]:   bearer_token_len
    ; [rbp - 88]:   body_ptr
    ; [rbp - 96]:   body_len
    ; [rbp - 104]:  keep_alive (1 or 0)
    ; [rbp - 112]:  auth_header_present (1 or 0)
    ; [rbp - 384]:  struct jwt_info (272 bytes)
    ; [rbp - 2432]: trimmed body scratch buffer (2048 bytes)
    sub rsp, 2440

    mov [rbp - 8], rdi
    mov [rbp - 16], rsi
    mov [rbp - 24], rdx
    mov [rbp - 32], rcx
    mov [rbp - 40], r8

    mov qword [rbp - 48], 0
    mov qword [rbp - 56], 0
    mov qword [rbp - 64], 0
    mov qword [rbp - 72], 0
    mov qword [rbp - 80], 0
    mov qword [rbp - 88], 0
    mov qword [rbp - 96], 0
    mov qword [rbp - 104], 1 ; default keep_alive = 1 (HTTP/1.1)

    ; Pressure-based eviction of idle keep-alives (escapes FD ceiling & loopback socket exhaustion)
    cmp dword [active_conns], 15000
    jl .keep_alive_ok
    mov qword [rbp - 104], 0
.keep_alive_ok:

    mov qword [rbp - 112], 0

    mov r12, rdi            ; req_buf
    mov r13, rsi            ; req_len

    ; 1. Parse Method
    cmp r13, 4
    jb .bad_request_400

    mov eax, [r12]
    cmp eax, 0x20544547     ; 'GET '
    je .is_get
    cmp eax, 0x54534f50     ; 'POST'
    je .check_post
    jmp .route_404          ; unknown method/route

.is_get:
    mov qword [rbp - 48], 1
    mov rbx, 4              ; start of path
    jmp .parse_path

.check_post:
    cmp r13, 5
    jb .bad_request_400
    cmp byte [r12 + 4], ' '
    jne .route_404
    mov qword [rbp - 48], 2
    mov rbx, 5              ; start of path

.parse_path:
    ; rbx points to start of path
    lea rax, [r12 + rbx]
    mov [rbp - 56], rax     ; path_ptr
    mov r14, rbx            ; path start idx

.scan_path_end:
    cmp rbx, r13
    jge .bad_request_400
    movzx eax, byte [r12 + rbx]
    cmp al, ' '
    je .found_path_end
    cmp al, '?'             ; query params cut-off
    je .found_path_end
    cmp al, 13
    je .found_path_end
    cmp al, 10
    je .found_path_end
    inc rbx
    jmp .scan_path_end

.found_path_end:
    mov rax, rbx
    sub rax, r14
    mov [rbp - 64], rax     ; path_len

    ; 2. Find headers and end of request line
    ; Scan for first \r\n
.scan_reqline_end:
    cmp rbx, r13
    jge .bad_request_400
    cmp byte [r12 + rbx], 10
    je .headers_start
    inc rbx
    jmp .scan_reqline_end

.headers_start:
    inc rbx                 ; rbx is at first header line

.parse_header_line:
    cmp rbx, r13
    jge .check_body
    ; Check if empty line (\r\n or \n)
    cmp byte [r12 + rbx], 10
    je .empty_line_lf
    cmp byte [r12 + rbx], 13
    jne .check_header_name
    lea rax, [rbx + 1]
    cmp rax, r13
    jge .check_body
    cmp byte [r12 + rbx + 1], 10
    je .empty_line_crlf
    jmp .check_header_name

.empty_line_crlf:
    add rbx, 2
    jmp .body_starts
.empty_line_lf:
    inc rbx
    jmp .body_starts

.check_header_name:
    ; Check for "Authorization:" (case-insensitive prefix "auth")
    mov rax, r13
    sub rax, rbx
    cmp rax, 14
    jl .skip_header_line

    ; Quick check 'a' or 'A'
    movzx eax, byte [r12 + rbx]
    or al, 0x20
    cmp al, 'a'
    jne .check_conn_hdr

    ; Match "authorization:"
    ; 'a','u','t','h' = 0x68747561
    mov eax, [r12 + rbx]
    or eax, 0x20202020
    cmp eax, 0x68747561
    jne .check_conn_hdr

    ; Found Authorization header!
    mov qword [rbp - 112], 1 ; auth header present

    ; Find ':'
    mov r14, rbx
.find_auth_colon:
    cmp r14, r13
    jge .skip_header_line
    cmp byte [r12 + r14], ':'
    je .found_auth_colon
    cmp byte [r12 + r14], 10
    je .skip_header_line
    inc r14
    jmp .find_auth_colon

.found_auth_colon:
    inc r14
    ; skip spaces
.skip_auth_ws:
    cmp r14, r13
    jge .skip_header_line
    cmp byte [r12 + r14], ' '
    jne .check_bearer
    inc r14
    jmp .skip_auth_ws

.check_bearer:
    ; Check "Bearer " (7 chars)
    mov rax, r13
    sub rax, r14
    cmp rax, 7
    jl .skip_header_line

    mov eax, [r12 + r14]
    cmp eax, 0x72616542     ; 'B','e','a','r'
    jne .skip_header_line
    cmp byte [r12 + r14 + 4], 'e'
    jne .skip_header_line
    cmp byte [r12 + r14 + 5], 'r'
    jne .skip_header_line
    cmp byte [r12 + r14 + 6], ' '
    jne .skip_header_line

    add r14, 7              ; start of token
    lea rax, [r12 + r14]
    mov [rbp - 72], rax     ; bearer_token_ptr
    mov r15, r14

.scan_token_end:
    cmp r14, r13
    jge .save_token_len
    movzx eax, byte [r12 + r14]
    cmp al, ' '
    je .save_token_len
    cmp al, 13
    je .save_token_len
    cmp al, 10
    je .save_token_len
    inc r14
    jmp .scan_token_end

.save_token_len:
    mov rax, r14
    sub rax, r15
    mov [rbp - 80], rax     ; bearer_token_len
    jmp .skip_header_line

.check_conn_hdr:
    ; Check "Connection: close"
    movzx eax, byte [r12 + rbx]
    or al, 0x20
    cmp al, 'c'
    jne .skip_header_line
    mov eax, [r12 + rbx]
    or eax, 0x20202020
    cmp eax, 0x6e6e6f63     ; 'c','o','n','n'
    jne .skip_header_line
    ; Find 'close' in this line
    mov r14, rbx
.scan_close:
    cmp r14, r13
    jge .skip_header_line
    cmp byte [r12 + r14], 10
    je .skip_header_line
    mov eax, [r12 + r14]
    or eax, 0x20202020
    cmp eax, 0x736f6c63     ; 'c','l','o','s'
    jne .next_close
    cmp byte [r12 + r14 + 4], 'e'
    jne .next_close
    mov qword [rbp - 104], 0 ; keep_alive = 0
    jmp .skip_header_line
.next_close:
    inc r14
    jmp .scan_close

.skip_header_line:
    ; advance rbx to next line after \n
    cmp rbx, r13
    jge .check_body
    cmp byte [r12 + rbx], 10
    je .next_hdr_line
    inc rbx
    jmp .skip_header_line
.next_hdr_line:
    inc rbx
    jmp .parse_header_line

.body_starts:
    lea rax, [r12 + rbx]
    mov [rbp - 88], rax     ; body_ptr
    mov rax, r13
    sub rax, rbx
    mov [rbp - 96], rax     ; body_len

.check_body:
    ; 3. Dispatch Routes
    mov r14, [rbp - 56]     ; path_ptr
    mov r15, [rbp - 64]     ; path_len

    ; -------------------------------------------------------------------------
    ; Route: GET /health (len=7)
    ; -------------------------------------------------------------------------
    cmp qword [rbp - 48], 1 ; GET?
    jne .not_get_health
    cmp r15, 7
    jne .not_get_health
    mov eax, [r14]
    cmp eax, 0x6165682f     ; '/hea'
    jne .not_get_health
    cmp byte [r14 + 4], 'l'
    jne .not_get_health
    cmp byte [r14 + 5], 't'
    jne .not_get_health
    cmp byte [r14 + 6], 'h'
    jne .not_get_health

    ; Call db_check_health
    call db_check_health
    test eax, eax
    jz .health_degraded

    ; Healthy! Calculate uptime_s
    sub rsp, 16
    xor edi, edi            ; CLOCK_REALTIME or CLOCK_MONOTONIC (1)
    mov edi, 1              ; CLOCK_MONOTONIC
    mov rsi, rsp
    call clock_gettime
    mov rax, [rsp]          ; sec
    sub rax, [server_start_time]
    add rsp, 16

    ; Format: {"status":"ok","db":"ok","uptime_s":<uptime>}
    lea rdi, [scratch_body_buf]
    lea rsi, [health_prefix]
    mov rdx, health_prefix_len
    call append
    mov rbx, rdi

    mov rdi, rax            ; uptime_s
    mov rsi, rbx
    call u64_to_str
    add rbx, rax

    mov byte [rbx], '}'
    inc rbx

    lea rax, [scratch_body_buf]
    mov rdx, rbx
    sub rdx, rax            ; body len
    mov rdi, 200
    lea rsi, [scratch_body_buf]
    mov rcx, [rbp - 104]    ; keep_alive
    mov r8, [rbp - 24]      ; resp_buf
    mov r9, [rbp - 32]      ; out_resp_len
    call build_response
    jmp .done

.health_degraded:
    mov rdi, 503
    lea rsi, [err_health_degraded]
    mov rdx, err_health_degraded_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.not_get_health:

    ; -------------------------------------------------------------------------
    ; Route: GET /feed (len=5)
    ; -------------------------------------------------------------------------
    cmp qword [rbp - 48], 1 ; GET?
    jne .not_get_feed
    cmp r15, 5
    jne .not_get_feed
    mov eax, [r14]
    cmp eax, 0x6565662f     ; '/fee'
    jne .not_get_feed
    cmp byte [r14 + 4], 'd'
    jne .not_get_feed

    lea rdi, [scratch_body_buf]  ; scratch body
    lea rsi, [scratch_body_len]  ; out_len
    call db_get_feed

    mov rdi, 200
    lea rsi, [scratch_body_buf]
    mov rdx, [scratch_body_len]
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.not_get_feed:

    ; -------------------------------------------------------------------------
    ; Check if path starts with "/posts"
    ; -------------------------------------------------------------------------
    cmp r15, 6
    jl .route_404
    mov eax, [r14]
    cmp eax, 0x736f702f     ; '/pos'
    jne .route_404
    cmp byte [r14 + 4], 't'
    jne .route_404
    cmp byte [r14 + 5], 's'
    jne .route_404

    ; If exactly "/posts"
    cmp r15, 6
    je .handle_exact_posts

    ; Must have '/' after "/posts" -> "/posts/..."
    cmp byte [r14 + 6], '/'
    jne .route_404

    ; Sub-routes under /posts/:
    ; 1) POST /posts/:id/like
    ; 2) GET /posts/:id
    cmp qword [rbp - 48], 2 ; POST?
    je .handle_post_subroute

    ; GET /posts/:id
    cmp qword [rbp - 48], 1 ; GET?
    jne .route_404

    ; Extract id: [r14 + 7 .. r14 + r15]
    lea rdi, [r14 + 7]
    mov rsi, r15
    sub rsi, 7              ; id string len
    sub rsp, 8
    mov rdx, rsp            ; out uint64
    call str_to_u64
    test eax, eax
    jz .invalid_post_id_pop

    pop rdi                 ; post_id
    lea rsi, [scratch_body_buf]  ; scratch body
    lea rdx, [scratch_body_len]  ; out_len
    call db_get_post
    test eax, eax
    jnz .post_not_found_404

    ; Found post!
    mov rdi, 200
    lea rsi, [scratch_body_buf]
    mov rdx, [scratch_body_len]
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.invalid_post_id_pop:
    add rsp, 8
.invalid_post_id_400:
    mov rdi, 400
    lea rsi, [err_invalid_post_id]
    mov rdx, err_invalid_post_id_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.post_not_found_404:
    mov rdi, 404
    lea rsi, [err_post_not_found]
    mov rdx, err_post_not_found_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.handle_exact_posts:
    ; POST /posts
    cmp qword [rbp - 48], 2 ; POST?
    jne .route_404

    ; Check Auth
    mov rdi, [rbp - 72]     ; bearer_token_ptr
    mov rsi, [rbp - 80]     ; bearer_token_len
    lea rdx, [rbp - 384]   ; jwt_info
    call check_and_verify_auth
    test eax, eax
    jnz .auth_failed

    ; Parse JSON body
    mov rdi, [rbp - 88]     ; body_ptr
    mov rsi, [rbp - 96]     ; body_len
    lea rdx, [rbp - 2432]   ; trimmed body buf
    lea rcx, [scratch_body_len]  ; out_len
    call json_parse_body
    cmp eax, 1
    je .malformed_json_400
    cmp eax, 2
    je .body_required_400
    cmp eax, 3
    je .body_too_long_400

    ; Valid body! Insert into DB
    mov rdi, [rbp - 384 + jwt_info.user_id]
    lea rsi, [rbp - 384 + jwt_info.username]
    lea rdx, [rbp - 2432]   ; trimmed body
    mov rcx, [scratch_body_len]  ; trimmed len
    lea r8, [scratch_body_buf]   ; scratch body
    lea r9, [scratch_body_len]   ; out_len
    call db_create_post
    test eax, eax
    jnz .internal_error_500

    ; 201 Created!
    mov rdi, 201
    lea rsi, [scratch_body_buf]
    mov rdx, [scratch_body_len]
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.handle_post_subroute:
    ; Check if path ends with "/like"
    ; Path format: /posts/<id>/like (min len = 7 + 1 + 5 = 13)
    cmp r15, 13
    jl .route_404

    ; Check "/like" suffix
    mov rax, r15
    sub rax, 5
    cmp byte [r14 + rax], '/'
    jne .route_404
    cmp byte [r14 + rax + 1], 'l'
    jne .route_404
    cmp byte [r14 + rax + 2], 'i'
    jne .route_404
    cmp byte [r14 + rax + 3], 'k'
    jne .route_404
    cmp byte [r14 + rax + 4], 'e'
    jne .route_404

    ; -------------------------------------------------------------------------
    ; POST /posts/:id/like
    ; CRITICAL: CHECK AUTH FIRST BEFORE VALIDATING ID! (SPEC & Test 83 requirement)
    ; -------------------------------------------------------------------------
    mov rdi, [rbp - 72]     ; bearer_token_ptr
    mov rsi, [rbp - 80]     ; bearer_token_len
    lea rdx, [rbp - 384]   ; jwt_info
    call check_and_verify_auth
    test eax, eax
    jnz .auth_failed

    ; Auth passed! NOW validate :id
    ; id is between [r14 + 7] and [r14 + rax]
    lea rdi, [r14 + 7]
    mov rsi, r15
    sub rsi, 12             ; 7 ("/posts/") + 5 ("/like")
    sub rsp, 8
    mov rdx, rsp
    call str_to_u64
    test eax, eax
    jz .invalid_post_id_pop

    pop rsi                 ; post_id
    mov rdi, [rbp - 384 + jwt_info.user_id]
    lea rdx, [scratch_body_buf]  ; scratch body
    lea rcx, [scratch_body_len]  ; out_len
    call db_like_post

    cmp eax, 201
    je .like_first
    cmp eax, 200
    je .like_repeat
    cmp eax, 404
    je .post_not_found_404
    jmp .internal_error_500

.like_first:
    mov rdi, 201
    lea rsi, [scratch_body_buf]
    mov rdx, [scratch_body_len]
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.like_repeat:
    mov rdi, 200
    lea rsi, [scratch_body_buf]
    mov rdx, [scratch_body_len]
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

; -----------------------------------------------------------------------------
; Error response helpers
; -----------------------------------------------------------------------------
.auth_failed:
    ; eax contains: 1=missing token, 2=invalid/expired, 3=invalid payload
    cmp eax, 1
    je .missing_token_401
    cmp eax, 2
    je .invalid_token_401
    jmp .invalid_payload_401

.missing_token_401:
    mov rdi, 401
    lea rsi, [err_missing_token]
    mov rdx, err_missing_token_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.invalid_token_401:
    mov rdi, 401
    lea rsi, [err_invalid_token]
    mov rdx, err_invalid_token_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.invalid_payload_401:
    mov rdi, 401
    lea rsi, [err_invalid_payload]
    mov rdx, err_invalid_payload_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.malformed_json_400:
    mov rdi, 400
    lea rsi, [err_malformed_json]
    mov rdx, err_malformed_json_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.body_required_400:
    mov rdi, 400
    lea rsi, [err_body_required]
    mov rdx, err_body_required_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.body_too_long_400:
    mov rdi, 400
    lea rsi, [err_body_too_long]
    mov rdx, err_body_too_long_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.route_404:
    mov rdi, 404
    lea rsi, [err_not_found]
    mov rdx, err_not_found_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.bad_request_400:
    mov rdi, 400
    lea rsi, [err_not_found]
    mov rdx, err_not_found_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.internal_error_500:
    mov rdi, 500
    lea rsi, [err_internal]
    mov rdx, err_internal_len
    mov rcx, [rbp - 104]
    mov r8, [rbp - 24]
    mov r9, [rbp - 32]
    call build_response
    jmp .done

.done:
    ; set out_keep_alive
    mov rax, [rbp - 104]
    mov r8, [rbp - 40]
    mov [r8], eax

    leave
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; =============================================================================
; Internal helper: check_and_verify_auth(bearer_ptr [rdi], bearer_len [rsi], out_jwt_info [rdx])
; Returns: eax = 0 (success, jwt_info filled)
;          eax = 1 (missing bearer token)
;          eax = 2 (invalid or expired token)
;          eax = 3 (invalid token payload)
; =============================================================================
check_and_verify_auth:
    test rdi, rdi
    jz .missing
    test rsi, rsi
    jz .missing

    push rbx
    push rbp
    mov rbp, rsp

    mov r8, rdx             ; out_info
    lea rdx, [jwt_secret_buf]
    mov rcx, [jwt_secret_len]
    call jwt_verify
    test eax, eax
    jz .ok
    cmp eax, 1
    je .invalid_token
    ; eax == 2 -> invalid payload
    mov eax, 3
    leave
    pop rbx
    ret

.ok:
    xor eax, eax
    leave
    pop rbx
    ret

.invalid_token:
    mov eax, 2
    leave
    pop rbx
    ret

.missing:
    mov eax, 1
    ret
