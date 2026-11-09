; Reads through SQLite's B-tree cursors (see shim.c). Records are decoded here, straight from the
; bytes SQLite hands back, and the JSON is written from them. Every value comes from rows read
; while the request is served, inside the pass's transaction (rule 5).
%include "defs.inc"

extern td_begin, td_cursor, td_seek, td_seek_ge, td_first, td_next, td_payload, td_pv
extern td_next_after, td_feed_sql_step
extern resp_begin, resp_end, w_int, w_esc, fcopy

global db_init, db_begin, db_feed, db_post, db_post_exists, g_in_tx

; feed row
%define FR_ID    0
%define FR_UID   8
%define FR_LIKES 16
%define FR_BODY  24
%define FR_BLEN  32
%define FR_CA    40
%define FR_CALEN 48
%define FR_AU    56
%define FR_AULEN 64
%define FR_SIZE  80

; decoded column: type, data pointer, data length
%define CO_T 0
%define CO_P 8
%define CO_N 16

%define TEXT_CAP (256 * 1024)

section .rodata
SLEN: db 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0
LIT L_POSTS_OPEN, '{"posts":['
LIT L_CLOSE_ARR,  ']}'
LIT L_ID,         '{"id":'
LIT L_BODY,       ',"body":"'
LIT L_CA,         '","created_at":"'
LIT L_AU,         '","author":"'
LIT L_LC,         '","like_count":'
LIT L_POST_OPEN,  '{"post":'

section .bss
alignb 16
g_cur:    resq 4
g_in_tx:  resq 1
g_plen:   resq 1
g_cols:   resb 4 * 24
g_ord:    resq FEED_LEN
g_pv:     resb 48
g_rows:   resb FEED_LEN * FR_SIZE
g_counts: resd RANGE_SPAN + 8
g_sqlrow: resq 8
g_tpos:   resq 1
g_text:   resb TEXT_CAP

; td_pv layout
%define PV_DATA 0
%define PV_CIDX 8
%define PV_END  16
%define PV_IX   24
%define PV_NC   28
%define PV_HDR  32
%define PV_MASK 36

section .text

db_init:
  push rbx
  xor ebx, ebx
.l:
  mov edi, ebx
  call td_cursor
  lea rcx, [g_cur]
  mov [rcx + rbx * 8], rax
  inc ebx
  cmp ebx, 4
  jb .l
  pop rbx
  ret

; Joins the pass's transaction: eax 0 ok, -1 lost.
db_begin:
  mov qword [g_in_tx], 1
  jmp td_begin

; ---- record decoding ----------------------------------------------------------------------------

; varint: rdi = p, rsi = end -> rax = value, rdi = after it (0 if it runs past end)
varint:
  cmp rdi, rsi
  jae .err
  movzx eax, byte [rdi]
  inc rdi
  test al, al
  jns .r
  and eax, 0x7f
  mov ecx, 1
.l:
  cmp rdi, rsi
  jae .err
  movzx edx, byte [rdi]
  inc rdi
  cmp ecx, 8
  je .ninth
  shl rax, 7
  mov r8d, edx
  and r8d, 0x7f
  or rax, r8
  inc ecx
  test dl, dl
  js .l
.r:
  ret
.ninth:
  shl rax, 8
  or rax, rdx
  ret
.err:
  xor edi, edi
  ret

; rec_cols(rdi = record, rsi = len, ecx = n): the first n columns into g_cols. eax 0 ok, -1 bad.
rec_cols:
  push rbx
  push r12
  push r13
  push r14
  push r15
  lea r13, [rdi + rsi]           ; end of record
  mov r12, rdi
  mov r15d, ecx
  movzx eax, byte [rdi]          ; header size (one byte unless the header is huge)
  test al, al
  js .hlong
  inc rdi
  jmp .hok
.hlong:
  mov rsi, r13
  call varint
  test rdi, rdi
  jz .bad
.hok:
  lea rbx, [r12 + rax]           ; end of header = start of data
  cmp rbx, r13
  ja .bad
  mov r14, rbx                   ; data cursor
  lea r12, [g_cols]
.col:
  test r15d, r15d
  jz .ok
  cmp rdi, rbx
  jae .bad
  movzx eax, byte [rdi]          ; serial type: one byte for anything up to 57 bytes of text
  test al, al
  js .tlong
  inc rdi
  jmp .tok
.tlong:
  mov rsi, rbx
  call varint
  test rdi, rdi
  jz .bad
.tok:
  mov [r12 + CO_T], rax
  ; serial type -> length
  cmp rax, 12
  jae .blob
  lea rcx, [SLEN]
  movzx ecx, byte [rcx + rax]
  jmp .len
.blob:
  lea rcx, [rax - 12]
  shr rcx, 1
.len:
  mov [r12 + CO_P], r14
  mov [r12 + CO_N], rcx
  add r14, rcx
  cmp r14, r13
  ja .bad
  add r12, 24
  dec r15d
  jmp .col
.ok:
  xor eax, eax
  jmp .r
.bad:
  mov eax, -1
.r:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; col_int(rdi = column) -> rax = value, edx = 0 ok / -1 not an integer
col_int:
  mov rcx, [rdi + CO_T]
  cmp rcx, 8
  je .zero
  cmp rcx, 9
  je .one
  dec rcx
  cmp rcx, 5
  ja .bad
  mov rsi, [rdi + CO_P]
  mov rcx, [rdi + CO_N]
  movsx rax, byte [rsi]
  mov r8d, 1
.l:
  cmp r8, rcx
  jae .ok
  shl rax, 8
  movzx edx, byte [rsi + r8]
  or rax, rdx
  inc r8
  jmp .l
.zero:
  xor eax, eax
  jmp .ok
.one:
  mov eax, 1
.ok:
  xor edx, edx
  ret
.bad:
  mov edx, -1
  ret

; is column rdi text? ZF clear (jnz) if not
%macro NOT_TEXT 1
  mov rax, [%1 + CO_T]
  cmp rax, 13
  jb %%no
  test al, 1
  jz %%no
  xor eax, eax                   ; ZF = 1: text
  jmp %%d
%%no:
  or eax, 1                      ; ZF = 0
%%d:
%endmacro

; tcopy(rsi = src, rdx = len) -> rax = its copy in g_text, or 0 if the buffer is full.
; Keeps 16 bytes free after every copy: w_esc may read that far past a string.
tcopy:
  push rbx
  mov rbx, [g_tpos]
  lea rax, [g_text + TEXT_CAP - 16]
  lea rcx, [rbx + rdx]
  cmp rcx, rax
  ja .full
  mov [g_tpos], rcx
  mov rdi, rbx
  call fcopy
  mov rax, rbx
  pop rbx
  ret
.full:
  xor eax, eax
  pop rbx
  ret

; payload of cursor rdi -> rax = record, rsi = length (rax 0 on error)
%macro PAYLOAD 1
  mov rdi, [g_cur + 8 * %1]
  lea rsi, [g_plen]
  call td_payload
  mov esi, [g_plen]
%endmacro

; ---- likes ------------------------------------------------------------------------------------

; count_range(rdi = lo, rsi = hi, rdx = counts): adds 1 to counts[post_id - lo] for every entry of
; likes_post_id_idx with lo <= post_id <= hi, decoding a leaf's cells in one go.
; eax 0 ok, -1 error, 1 a cell not in the expected form (the caller counts another way).
count_range:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12, rdi                   ; lo
  mov r13, rsi                   ; hi
  mov r14, rdx                   ; counts
  mov rsi, rdi
  mov rdi, [g_cur + 24]
  call td_seek_ge
  test eax, eax
  jle .ret                       ; 0 no entries (ok), -1 error
.page:
  mov rdi, [g_cur + 24]
  lea rsi, [g_pv]
  call td_pv
  lea rbx, [g_pv]
  mov r15d, [rbx + PV_IX]
.cell:
  cmp r15d, [rbx + PV_NC]
  jae .next
  mov rax, [rbx + PV_CIDX]
  movzx eax, word [rax + r15 * 2]
  xchg al, ah                    ; big-endian offset
  and eax, [rbx + PV_MASK]
  add rax, [rbx + PV_DATA]
  mov ecx, [rbx + PV_HDR]
  add rax, rcx                   ; the cell
  movzx ecx, byte [rax]          ; payload size (one-byte varint only)
  cmp ecx, 0x80
  jae .odd
  cmp ecx, 3
  jb .odd
  lea rdi, [rax + 1]             ; record
  lea rdx, [rdi + rcx]
  cmp rdx, [rbx + PV_END]
  ja .odd
  movzx edx, byte [rdi]          ; header size
  movzx r8d, byte [rdi + 1]      ; serial type of post_id
  cmp edx, ecx
  jae .odd
  cmp edx, 3
  jb .odd
  cmp r8d, 8
  je .v0
  cmp r8d, 9
  je .v1
  lea r9d, [r8d - 1]
  cmp r9d, 5
  ja .odd
  lea r9, [SLEN]
  movzx r9d, byte [r9 + r8]      ; value bytes
  lea r10d, [edx + r9d]
  cmp r10d, ecx
  ja .odd
  add rdi, rdx                   ; value
  movsx rax, byte [rdi]
  mov r10d, 1
.be:
  cmp r10d, r9d
  jae .have
  shl rax, 8
  movzx r11d, byte [rdi + r10]
  or rax, r11
  inc r10d
  jmp .be
.v0:
  xor eax, eax
  jmp .have
.v1:
  mov eax, 1
.have:
  cmp rax, r13
  jg .done
  cmp rax, r12
  jl .skip
  sub rax, r12
  inc dword [r14 + rax * 4]
.skip:
  inc r15d
  jmp .cell
.next:
  mov rdi, [g_cur + 24]
  mov esi, r15d
  dec esi
  call td_next_after
  test eax, eax
  jg .page
  jmp .ret                       ; 0 end (ok), -1 error
.done:
  xor eax, eax
  jmp .ret
.odd:
  mov eax, 1
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; count_slow(rdi = id) -> rax = likes of post id, decoding each entry as a record; -1 on error
count_slow:
  push rbx
  push r12
  push r13
  mov r12, rdi
  xor r13d, r13d
  mov rsi, rdi
  mov rdi, [g_cur + 24]
  call td_seek_ge
.l:
  cmp eax, 1
  jne .end
  PAYLOAD 3
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 1
  call rec_cols
  test eax, eax
  jnz .err
  lea rdi, [g_cols]
  call col_int
  test edx, edx
  jnz .err
  cmp rax, r12
  jne .done
  inc r13
  mov rdi, [g_cur + 24]
  call td_next
  jmp .l
.end:
  test eax, eax
  js .err
.done:
  mov rax, r13
  jmp .r
.err:
  mov rax, -1
.r:
  pop r13
  pop r12
  pop rbx
  ret

; count_one(rdi = id) -> rax = likes, -1 on error
count_one:
  push rbx
  mov rbx, rdi
  mov dword [g_counts], 0
  mov rsi, rdi
  lea rdx, [g_counts]
  call count_range
  test eax, eax
  jz .ok
  js .err
  mov rdi, rbx
  call count_slow
  pop rbx
  ret
.ok:
  mov eax, [g_counts]
  pop rbx
  ret
.err:
  mov rax, -1
  pop rbx
  ret

; ---- JSON for rows ----------------------------------------------------------------------------

; w_post(rdi = dst, rsi = row) -> rax = end: {"id":..,"body":..,"created_at":..,"author":..,"like_count":..}
w_post:
  push rbx
  mov rbx, rsi
  WLIT L_ID
  mov rsi, [rbx + FR_ID]
  call w_int
  mov rdi, rax
  WLIT L_BODY
  mov rsi, [rbx + FR_BODY]
  mov rdx, [rbx + FR_BLEN]
  call w_esc
  mov rdi, rax
  WLIT L_CA
  mov rsi, [rbx + FR_CA]
  mov rdx, [rbx + FR_CALEN]
  call w_esc
  mov rdi, rax
  WLIT L_AU
  mov rsi, [rbx + FR_AU]
  mov rdx, [rbx + FR_AULEN]
  call w_esc
  mov rdi, rax
  WLIT L_LC
  mov rsi, [rbx + FR_LIKES]
  call w_int
  mov byte [rax], '}'
  inc rax
  pop rbx
  ret

; feed_json(edi = rows): the 200 response for the rows in g_rows. eax 0, -1 if out of memory.
feed_json:
  push rbx
  push r12
  push r13
  mov r12d, edi
  mov rsi, [g_tpos]
  lea rax, [g_text]
  sub rsi, rax
  imul rsi, rsi, 6
  imul eax, r12d, 160
  add rsi, rax
  add rsi, 32
  mov edi, 200
  call resp_begin
  test rax, rax
  jz .oom
  mov rdi, rax
  WLIT L_POSTS_OPEN
  xor r13d, r13d
  lea rbx, [g_rows]
.row:
  cmp r13d, r12d
  jae .end
  test r13d, r13d
  jz .first
  mov byte [rdi], ','
  inc rdi
.first:
  mov rsi, rbx
  call w_post
  mov rdi, rax
  add rbx, FR_SIZE
  inc r13d
  jmp .row
.end:
  WLIT L_CLOSE_ARR
  call resp_end
  xor eax, eax
  jmp .r
.oom:
  mov eax, -1
.r:
  pop r13
  pop r12
  pop rbx
  ret

; ---- GET /feed ----------------------------------------------------------------------------------

; db_feed() -> eax 0 (response written), -1 error (nothing written)
db_feed:
  push rbx
  push r12
  push r13
  push r14
  push r15
  call db_begin
  test eax, eax
  jnz .err
  lea rax, [g_text]
  mov [g_tpos], rax
  xor r12d, r12d                 ; rows
  ; 1. posts_created_at_id_idx from its first entry: (created_at DESC, id DESC) = newest first
  mov rdi, [g_cur + 16]
  call td_first
  mov r13d, eax
.idx:
  cmp r13d, 1
  jne .idx_end
  cmp r12d, FEED_LEN
  jae .idx_end
  PAYLOAD 2
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 2
  call rec_cols
  test eax, eax
  jnz .err
  lea r14, [g_cols]
  NOT_TEXT r14
  jnz .err
  imul ebx, r12d, FR_SIZE
  lea rax, [g_rows]
  add rbx, rax
  mov rsi, [r14 + CO_P]
  mov rdx, [r14 + CO_N]
  mov [rbx + FR_CALEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_CA], rax
  lea rdi, [r14 + 24]
  call col_int
  test edx, edx
  jnz .err
  mov [rbx + FR_ID], rax
  inc r12d
  mov rdi, [g_cur + 16]
  call td_next
  mov r13d, eax
  jmp .idx
.idx_end:
  test r13d, r13d
  js .err
  test r12d, r12d
  jz .write
  ; 2. the posts in ascending id order: consecutive ids are the next cell of the same leaf
  lea r14, [g_ord]
  ; newest-first is normally descending id: then ascending is the reverse
  mov ecx, 1
.chk:
  cmp ecx, r12d
  jae .rev
  imul eax, ecx, FR_SIZE
  lea rdx, [g_rows]
  mov r8, [rdx + rax + FR_ID]
  cmp [rdx + rax - FR_SIZE + FR_ID], r8
  jle .sort
  inc ecx
  jmp .chk
.rev:
  xor ecx, ecx
.rl:
  mov eax, r12d
  dec eax
  sub eax, ecx
  mov [r14 + rcx * 8], rax
  inc ecx
  cmp ecx, r12d
  jb .rl
  jmp .posts
.sort:
  ; insertion sort of row indexes by id
  xor ecx, ecx
.si:
  mov [r14 + rcx * 8], rcx
  inc ecx
  cmp ecx, r12d
  jb .si
  mov ecx, 1
.so:
  cmp ecx, r12d
  jae .posts
  mov r8, [r14 + rcx * 8]        ; key index
  imul eax, r8d, FR_SIZE
  lea rdx, [g_rows]
  mov r9, [rdx + rax + FR_ID]    ; key id
  mov r10d, ecx
.sh:
  test r10d, r10d
  jz .put
  mov r11, [r14 + r10 * 8 - 8]
  imul eax, r11d, FR_SIZE
  cmp [rdx + rax + FR_ID], r9
  jle .put
  mov [r14 + r10 * 8], r11
  dec r10d
  jmp .sh
.put:
  mov [r14 + r10 * 8], r8
  inc ecx
  jmp .so
.posts:
  xor r15d, r15d
.pl:
  cmp r15d, r12d
  jae .authors
  mov rax, [g_ord + r15 * 8]
  imul ebx, eax, FR_SIZE
  lea rax, [g_rows]
  add rbx, rax
  mov rdi, [g_cur]
  mov rsi, [rbx + FR_ID]
  call td_seek
  test eax, eax
  js .err
  jz .fallback                   ; index and table disagree
  PAYLOAD 0
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 3
  call rec_cols
  test eax, eax
  jnz .err
  lea rdi, [g_cols + 24]
  call col_int
  test edx, edx
  jnz .err
  mov [rbx + FR_UID], rax
  lea r14, [g_cols + 48]
  NOT_TEXT r14
  jnz .err
  mov rsi, [r14 + CO_P]
  mov rdx, [r14 + CO_N]
  mov [rbx + FR_BLEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_BODY], rax
  inc r15d
  jmp .pl
  ; 3. authors
.authors:
  xor r15d, r15d
  lea rbx, [g_rows]
.al:
  cmp r15d, r12d
  jae .likes
  mov rdi, [g_cur + 8]
  mov rsi, [rbx + FR_UID]
  call td_seek
  test eax, eax
  js .err
  jz .fallback                   ; a post without a user: the JOIN drops it
  PAYLOAD 1
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 2
  call rec_cols
  test eax, eax
  jnz .err
  lea r14, [g_cols + 24]
  NOT_TEXT r14
  jnz .err
  mov rsi, [r14 + CO_P]
  mov rdx, [r14 + CO_N]
  mov [rbx + FR_AULEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_AU], rax
  add rbx, FR_SIZE
  inc r15d
  jmp .al
  ; 4. like counts: one walk of likes_post_id_idx when the ids are close together
.likes:
  mov rax, [g_ord]
  imul eax, eax, FR_SIZE
  lea rdx, [g_rows]
  mov r13, [rdx + rax + FR_ID]   ; lo
  mov eax, r12d
  dec eax
  mov rax, [g_ord + rax * 8]
  imul eax, eax, FR_SIZE
  mov r14, [rdx + rax + FR_ID]   ; hi
  mov rax, r14
  sub rax, r13
  cmp rax, RANGE_SPAN
  ja .each
  lea rdi, [g_counts]
  lea rcx, [rax + 1]
  xor eax, eax
  rep stosd
  mov rdi, r13
  mov rsi, r14
  lea rdx, [g_counts]
  call count_range
  test eax, eax
  js .err
  jnz .slow
  xor r15d, r15d
  lea rbx, [g_rows]
.cl:
  cmp r15d, r12d
  jae .write
  mov rax, [rbx + FR_ID]
  sub rax, r13
  mov eax, [g_counts + rax * 4]
  mov [rbx + FR_LIKES], rax
  add rbx, FR_SIZE
  inc r15d
  jmp .cl
.slow:
  xor r15d, r15d
  lea rbx, [g_rows]
.sl:
  cmp r15d, r12d
  jae .write
  mov rdi, [rbx + FR_ID]
  call count_slow
  test rax, rax
  js .err
  mov [rbx + FR_LIKES], rax
  add rbx, FR_SIZE
  inc r15d
  jmp .sl
.each:
  xor r15d, r15d
  lea rbx, [g_rows]
.el:
  cmp r15d, r12d
  jae .write
  mov rdi, [rbx + FR_ID]
  call count_one
  test rax, rax
  js .err
  mov [rbx + FR_LIKES], rax
  add rbx, FR_SIZE
  inc r15d
  jmp .el
.write:
  mov edi, r12d
  call feed_json
  jmp .ret
.fallback:
  call feed_sql
  jmp .ret
.err:
  mov eax, -1
.ret:
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; The reference query, for data the B-tree plan doesn't expect.
feed_sql:
  push rbx
  push r12
  push r13
  lea rax, [g_text]
  mov [g_tpos], rax
  xor r12d, r12d
  lea rbx, [g_rows]
.l:
  cmp r12d, FEED_LEN
  jae .w
  lea rdi, [g_sqlrow]
  call td_feed_sql_step
  test eax, eax
  js .err
  jz .w
  lea r13, [g_sqlrow]
  mov rax, [r13]
  mov [rbx + FR_ID], rax
  mov rax, [r13 + 8]
  mov [rbx + FR_LIKES], rax
  mov rsi, [r13 + 16]
  mov rdx, [r13 + 24]
  mov [rbx + FR_BLEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_BODY], rax
  mov rsi, [r13 + 32]
  mov rdx, [r13 + 40]
  mov [rbx + FR_CALEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_CA], rax
  mov rsi, [r13 + 48]
  mov rdx, [r13 + 56]
  mov [rbx + FR_AULEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_AU], rax
  add rbx, FR_SIZE
  inc r12d
  jmp .l
.w:
  mov edi, r12d
  call feed_json
  jmp .r
.err:
  mov eax, -1
.r:
  pop r13
  pop r12
  pop rbx
  ret

; ---- GET /posts/:id -----------------------------------------------------------------------------

; db_post(rdi = id) -> eax 0 written, 1 no such post, -1 error. The transaction is begun.
db_post:
  push rbx
  push r12
  push r13
  lea rbx, [g_rows]
  mov [rbx + FR_ID], rdi
  mov rsi, rdi
  mov rdi, [g_cur]
  call td_seek
  test eax, eax
  js .err
  jz .missing
  PAYLOAD 0
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 4
  call rec_cols
  test eax, eax
  jnz .err
  lea rdi, [g_cols + 24]
  call col_int
  test edx, edx
  jnz .err
  mov [rbx + FR_UID], rax
  lea rax, [g_text]
  mov [g_tpos], rax
  lea r12, [g_cols + 48]
  NOT_TEXT r12
  jnz .err
  mov rsi, [r12 + CO_P]
  mov rdx, [r12 + CO_N]
  mov [rbx + FR_BLEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_BODY], rax
  lea r12, [g_cols + 72]
  NOT_TEXT r12
  jnz .err
  mov rsi, [r12 + CO_P]
  mov rdx, [r12 + CO_N]
  mov [rbx + FR_CALEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_CA], rax
  ; the author (the post's cell is no longer needed)
  mov rdi, [g_cur + 8]
  mov rsi, [rbx + FR_UID]
  call td_seek
  test eax, eax
  js .err
  jz .missing                    ; the JOIN finds no row
  PAYLOAD 1
  test rax, rax
  jz .err
  mov rdi, rax
  mov ecx, 2
  call rec_cols
  test eax, eax
  jnz .err
  lea r12, [g_cols + 24]
  NOT_TEXT r12
  jnz .err
  mov rsi, [r12 + CO_P]
  mov rdx, [r12 + CO_N]
  mov [rbx + FR_AULEN], rdx
  call tcopy
  test rax, rax
  jz .err
  mov [rbx + FR_AU], rax
  mov rdi, [rbx + FR_ID]
  call count_one
  test rax, rax
  js .err
  mov [rbx + FR_LIKES], rax
  ; {"post":{...}}
  mov rsi, [g_tpos]
  lea rax, [g_text]
  sub rsi, rax
  imul rsi, rsi, 6
  add rsi, 200
  mov edi, 200
  call resp_begin
  test rax, rax
  jz .err
  mov rdi, rax
  WLIT L_POST_OPEN
  mov rsi, rbx
  call w_post
  mov byte [rax], '}'
  lea rdi, [rax + 1]
  call resp_end
  xor eax, eax
  jmp .r
.missing:
  mov eax, 1
  jmp .r
.err:
  mov eax, -1
.r:
  pop r13
  pop r12
  pop rbx
  ret

; db_post_exists(rdi = id) -> eax 1 yes, 0 no, -1 error
db_post_exists:
  mov rsi, rdi
  mov rdi, [g_cur]
  jmp td_seek
