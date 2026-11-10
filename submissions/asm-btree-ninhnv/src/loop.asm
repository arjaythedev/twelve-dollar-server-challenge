; The io_uring event loop and main. One thread drives every ring; see README.md for the design.
%include "defs.inc"

extern getenv, signal, clock_gettime, dprintf, exit, atoi, inet_pton, strlen, calloc
extern getrlimit, setrlimit, memmove
extern td_open, td_commit, td_tick, td_lock_code, td_unlock_code
extern db_init, g_in_tx, hmac_init, http_parse, handle_request, resp_error
extern buf_give, part_append, part_free, g_out_slot, g_start_s, g_oom
extern E_BAD_REQUEST, E_BAD_REQUEST_len, E_INTERNAL, E_INTERNAL_len

global main

section .rodata
ENV_PATH:   db "SQLITE_PATH", 0
ENV_SECRET: db "JWT_SECRET", 0
ENV_HOST:   db "HOST", 0
ENV_PORT:   db "PORT", 0
DEF_PATH:   db "feed.db", 0
DEF_SECRET: db 0
DEF_HOST:   db "127.0.0.1", 0
DEF_PORT:   db "3000", 0
M_DB:       db "database: %s", 10, 0
M_HOST:     db "HOST must be an IPv4 address: %s", 10, 0
M_LISTEN:   db "listen %s:%d failed (%d)", 10, 0
M_URING:    db "io_uring setup failed", 10, 0
M_READY:    db "listening on %s:%d; io_uring, %d direct descriptors per ring, %ld KB of code locked", 10, 0
M_RING:     db "ring %d added", 10, 0
M_NORING:   db "cannot add a ring (%d rings)", 10, 0

section .bss
alignb 16
g_rings:      resq MAX_RINGS
g_nrings:     resq 1
g_listen:     resd LISTENERS
g_slots:      resq 1
g_locked:     resq 1             ; KB of code locked in RAM
g_touched:    resq 1
g_ntouched:   resq 1
g_rq:         resq 1
g_rqh:        resq 1
g_rqt:        resq 1
g_held:       resq 1
g_nheld:      resq 1
g_need_ring:  resq 1
g_spill_after: resq 1
g_ready:      resq 1             ; ready list (ring buffer of tags)
g_rdh:        resq 1
g_rdt:        resq 1
g_answers:    resq 1             ; reads this pass may still answer
g_whold:      resq 1             ; writes are held (checkpoint), as of the last td_tick
g_res:        resq 1             ; the completion being handled
g_cflags:     resq 1
g_req:        resb Q_SIZE
g_geta:       resq 3             ; io_uring_getevents_arg
g_wts:        resq 2             ; its timeout
ts:           resq 2
rl:           resq 2
errbuf:       resb 256
g_host:       resq 1
g_port:       resq 1
g_addr:       resd 1

section .text

; ---- small helpers ----------------------------------------------------------------------------

; conn pointer: %1 = dest, %2 = ring, %3 = slot (32-bit register)
%macro CONN 3
  mov %1, [%2 + R_CONNS]
  push rax
  mov eax, %3
  shl rax, C_SHIFT
  add %1, rax
  pop rax
%endmacro

; rax = monotonic milliseconds
now_ms:
  sub rsp, 8
  mov edi, 1
  lea rsi, [ts]
  call clock_gettime
  imul rax, [ts], 1000
  mov rcx, [ts + 8]
  push rax
  mov rax, rcx
  xor edx, edx
  mov ecx, 1000000
  div rcx
  pop rcx
  add rax, rcx
  add rsp, 8
  ret

; ring_enter(rdi = ring, esi = min_complete, edx = flags, rcx = ext arg or 0) -> rax
ring_enter:
  mov r8, rcx
  xor r9d, r9d
  test r8, r8
  jz .n
  mov r9d, 24
.n:
  mov r10d, edx
  mov edx, esi
  mov rax, [rdi + R_SQHEAD]
  mov esi, [rdi + R_STAIL]
  sub esi, [rax]                 ; queued, not yet consumed by the kernel
  mov edi, [rdi + R_FD]
  mov eax, SYS_io_uring_enter
  syscall
  ret

; sqe_get(rdi = ring) -> rax = a zeroed SQE, already counted in the tail
sqe_get:
  push rbx
  mov rbx, rdi
.again:
  mov eax, [rbx + R_STAIL]
  mov rcx, [rbx + R_SQHEAD]
  mov ecx, [rcx]
  mov edx, eax
  sub edx, ecx
  cmp edx, [rbx + R_SQENT]
  jb .room
  mov rdi, rbx                   ; full: hand what is queued to the kernel
  xor esi, esi
  mov edx, IORING_ENTER_GETEVENTS
  xor ecx, ecx
  call ring_enter
  jmp .again
.room:
  mov ecx, eax
  and ecx, [rbx + R_SQMASK]
  shl rcx, 6
  add rcx, [rbx + R_SQES]
  inc eax
  mov [rbx + R_STAIL], eax
  mov rdx, [rbx + R_SQTAIL]
  mov [rdx], eax                 ; the kernel reads SQEs only inside io_uring_enter, on this thread
  pxor xmm0, xmm0
  movdqu [rcx], xmm0
  movdqu [rcx + 16], xmm0
  movdqu [rcx + 32], xmm0
  movdqu [rcx + 48], xmm0
  mov rax, rcx
  pop rbx
  ret

; sqe_room(rdi = ring, esi = n): makes room for n entries in a row (a linked pair stays together)
sqe_room:
  push rbx
  mov rbx, rdi
  mov eax, [rbx + R_STAIL]
  mov rcx, [rbx + R_SQHEAD]
  sub eax, [rcx]
  add eax, esi
  cmp eax, [rbx + R_SQENT]
  jbe .r
  mov rdi, rbx
  xor esi, esi
  mov edx, IORING_ENTER_GETEVENTS
  xor ecx, ecx
  call ring_enter
.r:
  pop rbx
  ret

; rcx = completion tag from op %1, ring rbx, gen r13d, slot r12d
%macro MKUD 1
  movzx ecx, word [rbx + R_INDEX]
  shl rcx, 48
  mov rdx, (%1 << 56)
  or rcx, rdx
  mov edx, r13d
  shl rdx, 32
  or rcx, rdx
  mov edx, r12d
  or rcx, rdx
%endmacro

; arm_recv(rdi = ring, esi = slot, edx = gen): the multishot receive (one completion per request)
arm_recv:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12d, esi
  mov r13d, edx
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_RECV
  mov byte [rax + SQE_FLAGS], SQE_FIXED_FILE | SQE_BUFFER_SELECT
  mov word [rax + SQE_IOPRIO], IORING_RECV_MULTISHOT
  mov [rax + SQE_FD], r12d
  MKUD T_RECV
  mov [rax + SQE_USERDATA], rcx
  pop r13
  pop r12
  pop rbx
  ret

; arm_read(rdi = ring, esi = slot, edx = gen): one MSG_DONTWAIT receive, for a queued connection
arm_read:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12d, esi
  mov r13d, edx
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_RECV
  mov byte [rax + SQE_FLAGS], SQE_FIXED_FILE | SQE_BUFFER_SELECT
  mov dword [rax + SQE_OPFLAGS], MSG_DONTWAIT
  mov [rax + SQE_FD], r12d
  MKUD T_READ
  mov [rax + SQE_USERDATA], rcx
  pop r13
  pop r12
  pop rbx
  ret

; arm_send(rdi = ring, esi = slot): sends the rest of the connection's wire buffer
arm_send:
  push rbx
  push r12
  push r13
  push r14
  sub rsp, 8
  mov rbx, rdi
  mov r12d, esi
  CONN r14, rbx, r12d
  movzx r13d, word [r14 + C_GEN]
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_SEND
  mov byte [rax + SQE_FLAGS], SQE_FIXED_FILE
  mov [rax + SQE_FD], r12d
  mov dword [rax + SQE_OPFLAGS], MSG_NOSIGNAL
  mov rdx, [r14 + C_WIRE]
  mov ecx, [r14 + C_SENT]
  lea rsi, [rdx + B_DATA + rcx]
  mov [rax + SQE_ADDR], rsi
  mov esi, [rdx + B_LEN]
  sub esi, ecx
  mov [rax + SQE_LEN], esi
  MKUD T_SEND
  mov [rax + SQE_USERDATA], rcx
  add rsp, 8
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; arm_close(rdi = ring, esi = slot): a shutdown hard-linked to the close of the direct descriptor
arm_close:
  push rbx
  push r12
  push r13
  push r14
  sub rsp, 8
  mov rbx, rdi
  mov r12d, esi
  CONN r14, rbx, r12d
  or word [r14 + C_FLAGS], F_CLOSING
  movzx r13d, word [r14 + C_GEN]
  mov rdi, rbx
  mov esi, 2
  call sqe_room
  mov rdi, rbx
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_SHUTDOWN
  mov byte [rax + SQE_FLAGS], SQE_FIXED_FILE | SQE_IO_HARDLINK
  mov [rax + SQE_FD], r12d
  mov dword [rax + SQE_LEN], 2   ; SHUT_RDWR
  MKUD T_SHUTDOWN
  mov [rax + SQE_USERDATA], rcx
  mov rdi, rbx
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_CLOSE
  lea ecx, [r12d + 1]
  mov [rax + SQE_FILEIDX], ecx
  MKUD T_CLOSE
  mov [rax + SQE_USERDATA], rcx
  add rsp, 8
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; fill_accepts(rdi = ring): keeps depth single-shot accepts in flight, spread over the listeners
fill_accepts:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  xor r12d, r12d                 ; listener
.li:
  cmp r12d, LISTENERS
  jae .r
.more:
  mov eax, [rbx + R_DEPTH]
  shr eax, 3
  cmp [rbx + R_ACC + r12 * 4], eax
  jae .nextli
  mov rdi, rbx
  call sqe_get
  mov byte [rax + SQE_OPCODE], OP_ACCEPT
  lea rcx, [g_listen]
  mov ecx, [rcx + r12 * 4]
  mov [rax + SQE_FD], ecx
  mov dword [rax + SQE_FILEIDX], IORING_FILE_INDEX_ALLOC
  xor r13d, r13d
  MKUD T_ACCEPT
  mov [rax + SQE_USERDATA], rcx
  inc dword [rbx + R_ACC + r12 * 4]
  jmp .more
.nextli:
  inc r12d
  jmp .li
.r:
  pop r13
  pop r12
  pop rbx
  ret

; gives receive buffer %2 (32-bit reg, clobbered ok) back to ring %1's buffer ring (published later)
%macro RECYCLE 2
  mov eax, [%1 + R_BTAIL]
  and eax, BUF_COUNT - 1
  shl eax, 4
  add rax, [%1 + R_BRING]
  mov edx, %2
  mov [rax + 12], dx
  shl rdx, 11
  add rdx, [%1 + R_BMEM]
  mov [rax], rdx
  mov dword [rax + 8], BUF_SIZE
  inc dword [%1 + R_BTAIL]
%endmacro

%macro PUBLISH_BUFS 1
  mov eax, [%1 + R_BTAIL]
  mov rdx, [%1 + R_BRING]
  mov [rdx + 14], ax
%endmacro

; tag for the lists: ring << 48 | gen << 32 | slot -> rax; %1 ring, %2 slot (32-bit), %3 conn
%macro LTAG 3
  movzx eax, word [%1 + R_INDEX]
  shl rax, 48
  movzx edx, word [%3 + C_GEN]
  shl rdx, 32
  or rax, rdx
  mov edx, %2
  or rax, rdx
%endmacro

; mark(rdi = ring, esi = slot): puts the connection on this pass's touched list (once)
mark:
  CONN rcx, rdi, esi
  test word [rcx + C_FLAGS], F_INBATCH
  jnz .r
  or word [rcx + C_FLAGS], F_INBATCH
  mov rdx, [rcx + C_OUT]
  xor eax, eax
  test rdx, rdx
  jz .m
  mov eax, [rdx + B_LEN]
.m:
  mov [rcx + C_MARK], eax
  mov r8, rcx
  LTAG rdi, esi, r8
  mov rcx, [g_ntouched]
  mov rdx, [g_touched]
  mov [rdx + rcx * 8], rax
  inc rcx
  mov [g_ntouched], rcx
.r:
  ret

; queue_read(rdi = ring, esi = slot): on the read queue (once)
queue_read:
  CONN rcx, rdi, esi
  test word [rcx + C_FLAGS], F_QUEUED
  jnz .r
  or word [rcx + C_FLAGS], F_QUEUED
  mov r8, rcx
  LTAG rdi, esi, r8
  mov rcx, [g_rqt]
  mov rdx, rcx
  and rdx, LIST_CAP - 1
  mov r8, [g_rq]
  mov [r8 + rdx * 8], rax
  inc rcx
  mov [g_rqt], rcx
.r:
  ret

; finish_conn(rdi = ring, esi = slot): the client is done sending. A held write or bytes on the
; ready list are still owed answers: then it closes once they are answered.
finish_conn:
  CONN rcx, rdi, esi
  test word [rcx + C_FLAGS], F_HELD | F_READY
  jz .close
  or word [rcx + C_FLAGS], F_EOF
  ret
.close:
  or word [rcx + C_FLAGS], F_CLOSEAFT
  jmp mark

; ---- serving ----------------------------------------------------------------------------------

; on_data(rdi = ring, esi = slot, rdx = data, rcx = len): answered now if this pass has answers left
; and nothing is waiting on the ready list; otherwise kept on the connection for a later pass, in
; arrival order (drain first, oldest first: a backlog is never starved by fresh reads)
on_data:
  CONN r8, rdi, esi
  movzx eax, word [r8 + C_FLAGS]
  test eax, F_CLOSEAFT
  jnz .r
  test eax, F_HELD | F_READY
  jnz .keep
  cmp qword [g_answers], 0
  je .keep
  mov rax, [g_rdt]
  cmp rax, [g_rdh]
  jne .keep
  dec qword [g_answers]
  jmp serve
.keep:
  push rbx
  push r12
  push r13
  mov rbx, rdi
  mov r12d, esi
  mov r13, r8
  lea rdi, [r8 + C_PART]
  mov rsi, rdx
  mov rdx, rcx
  call part_append
  test word [r13 + C_FLAGS], F_HELD | F_READY
  jnz .kept
  or word [r13 + C_FLAGS], F_READY
  LTAG rbx, r12d, r13
  mov rcx, [g_rdt]
  mov rdx, rcx
  and rdx, LIST_CAP - 1
  mov r8, [g_ready]
  mov [r8 + rdx * 8], rax
  inc rcx
  mov [g_rdt], rcx
.kept:
  pop r13
  pop r12
  pop rbx
.r:
  ret

; serve(rdi = ring, esi = slot, rdx = data, rcx = len): answers every complete request in the
; connection's kept bytes plus data. During a checkpoint hold a POST stops it: that request and
; everything behind it stay on the connection, which goes on the held list.
serve:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 24                    ; [0] from_part
  mov rbx, rdi
  mov r12d, esi
  CONN rbp, rbx, r12d
  test word [rbp + C_FLAGS], F_CLOSEAFT
  jnz .ret
  mov r13, rdx
  mov r14, rcx
  mov qword [rsp], 0
  cmp qword [rbp + C_PART], 0
  je .parse
  test r14, r14
  jz .usepart
  lea rdi, [rbp + C_PART]
  mov rsi, r13
  mov rdx, r14
  call part_append
.usepart:
  mov rax, [rbp + C_PART]
  lea r13, [rax + B_DATA]
  mov r14d, [rax + B_LEN]
  mov qword [rsp], 1
.parse:
  xor r15d, r15d                 ; offset
.loop:
  cmp r15, r14
  jae .after
  lea rdi, [r13 + r15]
  mov rsi, r14
  sub rsi, r15
  lea rdx, [g_req]
  call http_parse
  test eax, eax
  jz .after
  js .bad
  cmp qword [g_req + Q_METHOD], 1
  jne .answer
  cmp qword [g_whold], 0
  je .answer
  test word [rbp + C_FLAGS], F_HELD
  jnz .after
  or word [rbp + C_FLAGS], F_HELD
  LTAG rbx, r12d, rbp
  mov rcx, [g_nheld]
  mov rdx, [g_held]
  mov [rdx + rcx * 8], rax
  inc rcx
  mov [g_nheld], rcx
  jmp .after
.answer:
  mov rdi, rbx
  mov esi, r12d
  call mark
  or word [rbp + C_FLAGS], F_ANSWERED
  lea rax, [rbp + C_OUT]
  mov [g_out_slot], rax
  mov qword [g_oom], 0
  lea rdi, [g_req]
  call handle_request
  add r15, [g_req + Q_USED]
  cmp qword [g_oom], 0
  je .loop
  ; out of memory: a request went unanswered, so nothing after it can be answered in order
  or word [rbp + C_FLAGS], F_CLOSEAFT
  mov r15, r14
  jmp .after
.bad:
  mov rdi, rbx
  mov esi, r12d
  call mark
  or word [rbp + C_FLAGS], F_ANSWERED | F_CLOSEAFT
  lea rax, [rbp + C_OUT]
  mov [g_out_slot], rax
  mov edi, 400
  lea rsi, [E_BAD_REQUEST]
  mov edx, E_BAD_REQUEST_len
  call resp_error
  mov r15, r14
.after:
  mov rdx, r14
  sub rdx, r15                   ; bytes left
  cmp qword [rsp], 0
  je .fromdata
  mov rdi, [rbp + C_PART]
  test rdx, rdx
  jnz .keep
  call part_free
  mov qword [rbp + C_PART], 0
  jmp .ret
.keep:
  test r15, r15
  jz .ret
  mov [rdi + B_LEN], edx
  add rdi, B_DATA
  lea rsi, [rdi + r15]
  call memmove
  jmp .ret
.fromdata:
  test rdx, rdx
  jz .ret
  lea rdi, [rbp + C_PART]
  lea rsi, [r13 + r15]
  call part_append
.ret:
  add rsp, 24
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; ---- completions ------------------------------------------------------------------------------

; drain(rdi = ring) -> eax = completions handled
drain:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov rbx, rdi
  mov rax, [rbx + R_CQHEAD]
  mov r14d, [rax]
  mov rax, [rbx + R_CQTAIL]
  mov r15d, [rax]
  xor ebp, ebp
.loop:
  cmp r14d, r15d
  je .end
  mov eax, r14d
  and eax, [rbx + R_CQMASK]
  shl rax, 4
  add rax, [rbx + R_CQES]
  mov rcx, [rax + CQE_USERDATA]
  movsxd rdx, dword [rax + CQE_RES]
  mov esi, [rax + CQE_FLAGS]
  mov [g_res], rdx
  mov [g_cflags], rsi
  inc r14d
  inc ebp
  mov rax, [rbx + R_CQHEAD]
  mov [rax], r14d
  mov r12d, ecx                  ; slot (listener for accepts)
  mov r13, rcx
  shr r13, 32
  and r13d, 0xffff               ; gen
  shr rcx, 56
  cmp ecx, T_RECV
  je .recv
  cmp ecx, T_SEND
  je .send
  cmp ecx, T_READ
  je .read
  cmp ecx, T_ACCEPT
  je .accept
  cmp ecx, T_CLOSE
  je .close
  jmp .loop

.accept:
  dec dword [rbx + R_ACC + r12 * 4]
  mov rax, [g_res]
  test rax, rax
  js .acc_err
  mov r12d, eax
  CONN r8, rbx, r12d
  movzx r13d, word [r8 + C_GEN]
  inc r13d
  and r13d, 0xffff
  pxor xmm0, xmm0
  movdqu [r8], xmm0
  movdqu [r8 + 16], xmm0
  movdqu [r8 + 32], xmm0
  movdqu [r8 + 48], xmm0
  mov [r8 + C_GEN], r13w
  mov word [r8 + C_FLAGS], F_OPEN
  mov rdi, rbx
  mov esi, r12d
  mov edx, r13d
  call arm_recv
  inc dword [rbx + R_OPEN]
  mov eax, [rbx + R_OPEN]
  add eax, [rbx + R_MARGIN]
  cmp rax, [g_slots]
  jb .loop
  mov byte [rbx + R_FULL], 1
  jmp .loop
.acc_err:
  cmp rax, -ENFILE
  jne .loop
  mov byte [rbx + R_FULL], 1
  jmp .loop

.recv:                           ; the multishot receive
  CONN r8, rbx, r12d
  cmp [r8 + C_GEN], r13w
  jne .stale
  movzx eax, word [r8 + C_FLAGS]
  and eax, F_OPEN | F_CLOSING
  cmp eax, F_OPEN
  jne .stale
  mov rax, [g_res]
  test rax, rax
  jle .recv_end
  test qword [g_cflags], IORING_CQE_F_BUFFER
  jz .requeue
  call .data
  test qword [g_cflags], IORING_CQE_F_MORE
  jnz .loop
.requeue:
  ; it ended (its buffers ran out): read the rest in bounded single-shot reads
  mov rdi, rbx
  mov esi, r12d
  call queue_read
  jmp .loop
.recv_end:
  cmp rax, -ENOBUFS
  je .requeue                    ; not re-armed now: re-arming every receive at once repeats the storm
.eof:
  call .recycle
  mov rdi, rbx
  mov esi, r12d
  call finish_conn
  jmp .loop

.read:                           ; a single-shot read of a queued connection
  dec dword [rbx + R_READS]
  CONN r8, rbx, r12d
  cmp [r8 + C_GEN], r13w
  jne .stale
  movzx eax, word [r8 + C_FLAGS]
  and eax, F_OPEN | F_CLOSING
  cmp eax, F_OPEN
  jne .stale
  and word [r8 + C_FLAGS], ~F_RECVING
  mov rax, [g_res]
  test rax, rax
  jle .read_end
  test qword [g_cflags], IORING_CQE_F_BUFFER
  jz .requeue
  call .data
  cmp qword [g_res], BUF_SIZE
  je .requeue                    ; it filled the buffer: there may be more
.rearm:
  mov rdi, rbx                   ; drained: back to the multishot receive
  mov esi, r12d
  mov edx, r13d
  call arm_recv
  jmp .loop
.read_end:
  cmp rax, -EAGAIN
  je .rearm
  cmp rax, -ENOBUFS
  je .requeue
  jmp .eof

.stale:
  call .recycle
  jmp .loop

.send:
  CONN r8, rbx, r12d
  cmp [r8 + C_GEN], r13w
  jne .loop
  test word [r8 + C_FLAGS], F_SENDING
  jz .loop
  mov rax, [g_res]
  test rax, rax
  js .send_err
  add [r8 + C_SENT], eax
  mov rdi, [r8 + C_WIRE]
  mov eax, [r8 + C_SENT]
  cmp eax, [rdi + B_LEN]
  jae .sent
  mov rdi, rbx
  mov esi, r12d
  call arm_send
  jmp .loop
.sent:
  and word [r8 + C_FLAGS], ~F_SENDING
  mov qword [r8 + C_WIRE], 0
  push r8
  push r8
  call buf_give
  pop r8
  pop r8
  test word [r8 + C_FLAGS], F_CLOSEAFT
  jnz .flush
  mov rdi, [r8 + C_OUT]
  test rdi, rdi
  jz .loop
  cmp dword [rdi + B_LEN], 0
  jne .flush
  mov qword [r8 + C_OUT], 0
  call buf_give
  jmp .loop
.flush:
  mov rdi, rbx                   ; sent after this pass commits, with its other responses
  mov esi, r12d
  call mark
  jmp .loop
.send_err:
  and word [r8 + C_FLAGS], ~F_SENDING
  or word [r8 + C_FLAGS], F_CLOSEAFT
  push r8
  push r8
  mov rdi, [r8 + C_WIRE]
  mov qword [r8 + C_WIRE], 0
  call buf_give
  mov r8, [rsp]
  mov rdi, [r8 + C_OUT]
  mov qword [r8 + C_OUT], 0
  call buf_give
  pop r8
  pop r8
  jmp .flush

.close:
  CONN r8, rbx, r12d
  cmp [r8 + C_GEN], r13w
  jne .close_room
  test word [r8 + C_FLAGS], F_OPEN
  jz .close_room
  push r8
  push r8
  mov rdi, [r8 + C_OUT]
  call buf_give
  mov r8, [rsp]
  mov rdi, [r8 + C_WIRE]
  call buf_give
  mov r8, [rsp]
  mov rdi, [r8 + C_PART]
  call part_free
  pop r8
  pop r8
  pxor xmm0, xmm0
  movdqu [r8], xmm0
  movdqu [r8 + 16], xmm0
  movdqu [r8 + 32], xmm0
  movdqu [r8 + 48], xmm0
  mov [r8 + C_GEN], r13w
  dec dword [rbx + R_OPEN]
.close_room:
  cmp byte [rbx + R_FULL], 0
  je .loop
  mov eax, [rbx + R_MARGIN]
  add eax, eax
  add eax, [rbx + R_OPEN]
  cmp rax, [g_slots]
  jae .loop
  mov byte [rbx + R_FULL], 0
  mov byte [rbx + R_ACCEPTING], 1
  jmp .loop

.end:
  PUBLISH_BUFS rbx
  cmp byte [rbx + R_ACCEPTING], 0
  je .ret
  cmp byte [rbx + R_FULL], 0
  je .fill
  mov byte [rbx + R_ACCEPTING], 0 ; move on; the accepts still in flight fit in the margin
  mov qword [g_need_ring], 1
  jmp .ret
.fill:
  mov rdi, rbx
  call fill_accepts
.ret:
  mov eax, ebp
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; the received bytes of this completion: answer them, then give the buffer back
; (called from drain with the stack one push deeper)
.data:
  sub rsp, 8
  mov rdi, rbx
  mov esi, r12d
  mov rdx, [g_cflags]
  shr edx, 16
  shl rdx, 11
  add rdx, [rbx + R_BMEM]
  mov rcx, [g_res]
  call on_data
  add rsp, 8
  ; fall through
.recycle:
  test qword [g_cflags], IORING_CQE_F_BUFFER
  jz .norec
  mov rcx, [g_cflags]
  shr ecx, 16
  RECYCLE rbx, ecx
.norec:
  ret

; ---- the pass ---------------------------------------------------------------------------------

; end_batch: commits the pass, then hands every response it produced to the kernel
end_batch:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov r15d, 1                    ; committed
  cmp qword [g_in_tx], 0
  je .tick
  mov qword [g_in_tx], 0
  call td_commit
  test eax, eax
  jz .tick
  xor r15d, r15d
.tick:
  call td_tick                   ; no transaction open: the only place a hold starts or ends
  mov eax, eax                   ; a C int: the upper half of rax is undefined
  mov [g_whold], rax
  xor r14d, r14d
.l:
  cmp r14, [g_ntouched]
  jae .done
  mov rax, [g_touched]
  mov rax, [rax + r14 * 8]
  inc r14
  mov rcx, rax
  shr rcx, 48
  lea rdx, [g_rings]
  mov rbx, [rdx + rcx * 8]       ; ring
  mov r12d, eax                  ; slot
  shr rax, 32
  CONN rbp, rbx, r12d
  cmp [rbp + C_GEN], ax
  jne .l
  test r15d, r15d
  jnz .ok
  test word [rbp + C_FLAGS], F_ANSWERED
  jz .ok
  ; no response from a rolled-back pass may claim success
  mov rdi, [rbp + C_OUT]
  test rdi, rdi
  jz .e500
  mov eax, [rbp + C_MARK]
  mov [rdi + B_LEN], eax
.e500:
  lea rax, [rbp + C_OUT]
  mov [g_out_slot], rax
  mov edi, 500
  lea rsi, [E_INTERNAL]
  mov edx, E_INTERNAL_len
  call resp_error
  or word [rbp + C_FLAGS], F_CLOSEAFT
.ok:
  and word [rbp + C_FLAGS], ~(F_INBATCH | F_ANSWERED)
  movzx eax, word [rbp + C_FLAGS]
  and eax, F_OPEN | F_CLOSING | F_SENDING
  cmp eax, F_OPEN
  jne .l
  mov rdi, [rbp + C_OUT]
  test rdi, rdi
  jz .noout
  cmp dword [rdi + B_LEN], 0
  je .empty
  mov [rbp + C_WIRE], rdi
  mov qword [rbp + C_OUT], 0
  mov dword [rbp + C_SENT], 0
  or word [rbp + C_FLAGS], F_SENDING
  mov rdi, rbx
  mov esi, r12d
  call arm_send
  jmp .l
.empty:
  mov qword [rbp + C_OUT], 0
  call buf_give
.noout:
  test word [rbp + C_FLAGS], F_CLOSEAFT
  jz .l
  mov rdi, rbx
  mov esi, r12d
  call arm_close
  jmp .l
.done:
  mov qword [g_ntouched], 0
  cmp qword [g_need_ring], 0
  je .r
  call now_ms
  cmp rax, [g_spill_after]
  jb .r
  mov rbx, rax
  call spill
  test eax, eax
  jz .later
  mov qword [g_need_ring], 0
  jmp .r
.later:
  add rbx, 1000                  ; retried once a second until some ring accepts again
  mov [g_spill_after], rbx
.r:
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; issue_reads(rdi = budget) -> rax = reads issued: one per queued connection, never more on a
; ring than it has receive buffers; the rest stay queued, in order
issue_reads:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  mov r15, rdi                   ; budget
  xor r14d, r14d                 ; issued
  mov r13, [g_rqt]
  sub r13, [g_rqh]               ; entries to look at
.l:
  test r13, r13
  jz .done
  cmp r14, r15
  jae .done
  dec r13
  mov rcx, [g_rqh]
  mov rdx, rcx
  and rdx, LIST_CAP - 1
  inc rcx
  mov [g_rqh], rcx
  mov rax, [g_rq]
  mov rax, [rax + rdx * 8]
  mov rcx, rax
  shr rcx, 48
  lea rdx, [g_rings]
  mov rbx, [rdx + rcx * 8]
  mov r12d, eax
  mov [rsp], rax
  shr rax, 32
  CONN rbp, rbx, r12d
  cmp [rbp + C_GEN], ax
  jne .l
  movzx ecx, word [rbp + C_FLAGS]
  and ecx, F_OPEN | F_CLOSING
  cmp ecx, F_OPEN
  jne .l
  test word [rbp + C_FLAGS], F_RECVING
  jnz .back
  cmp dword [rbx + R_READS], BUF_COUNT
  jae .back
  and word [rbp + C_FLAGS], ~F_QUEUED
  or word [rbp + C_FLAGS], F_RECVING
  inc dword [rbx + R_READS]
  inc r14
  mov rdi, rbx
  mov esi, r12d
  movzx edx, word [rbp + C_GEN]
  call arm_read
  jmp .l
.back:
  mov rax, [rsp]
  mov rcx, [g_rqt]
  mov rdx, rcx
  and rdx, LIST_CAP - 1
  mov r8, [g_rq]
  mov [r8 + rdx * 8], rax
  inc rcx
  mov [g_rqt], rcx
  jmp .l
.done:
  mov rax, r14
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; read_rounds: reads the queued connections in rounds. A MSG_DONTWAIT read completes inside the
; submit, so a round is one non-blocking enter per ring.
read_rounds:
  push rbx
  push r12
  push r13
  mov r12, READS_PER_PASS
.round:
  test r12, r12
  jz .r
  mov rax, [g_rqt]
  cmp rax, [g_rqh]
  je .r
  mov rdi, r12
  call issue_reads
  test rax, rax
  jz .r
  sub r12, rax
  xor r13d, r13d
.sub:
  cmp r13, [g_nrings]
  jae .drain
  lea rax, [g_rings]
  mov rbx, [rax + r13 * 8]
  mov rax, [rbx + R_SQHEAD]
  mov eax, [rax]
  cmp eax, [rbx + R_STAIL]
  je .nosub
  mov rdi, rbx
  xor esi, esi
  mov edx, IORING_ENTER_GETEVENTS
  xor ecx, ecx
  call ring_enter
.nosub:
  inc r13
  jmp .sub
.drain:
  xor r13d, r13d
.dl:
  cmp r13, [g_nrings]
  jae .round
  lea rax, [g_rings]
  mov rdi, [rax + r13 * 8]
  call drain
  inc r13
  jmp .dl
.r:
  pop r13
  pop r12
  pop rbx
  ret

; release_held: the hold is over; serves the writes it held (and what arrived behind them)
release_held:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
  xor r14d, r14d
.l:
  cmp r14, [g_nheld]
  jae .done
  mov rax, [g_held]
  mov rax, [rax + r14 * 8]
  inc r14
  mov rcx, rax
  shr rcx, 48
  lea rdx, [g_rings]
  mov rbx, [rdx + rcx * 8]
  mov r12d, eax
  shr rax, 32
  CONN rbp, rbx, r12d
  cmp [rbp + C_GEN], ax
  jne .l
  movzx ecx, word [rbp + C_FLAGS]
  and ecx, F_OPEN | F_CLOSING
  cmp ecx, F_OPEN
  jne .l
  and word [rbp + C_FLAGS], ~F_HELD
  mov rdi, rbx
  mov esi, r12d
  xor edx, edx
  xor ecx, ecx
  call serve
  movzx eax, word [rbp + C_FLAGS]
  and eax, F_EOF | F_HELD
  cmp eax, F_EOF
  jne .l
  or word [rbp + C_FLAGS], F_CLOSEAFT
  mov rdi, rbx
  mov esi, r12d
  call mark
  jmp .l
.done:
  mov qword [g_nheld], 0
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; run_ready: answers connections on the ready list, oldest first, while this pass has answers left
run_ready:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8
.l:
  cmp qword [g_answers], 0
  je .done
  mov rcx, [g_rdh]
  cmp rcx, [g_rdt]
  je .done
  mov rdx, rcx
  and rdx, LIST_CAP - 1
  inc rcx
  mov [g_rdh], rcx
  mov rax, [g_ready]
  mov rax, [rax + rdx * 8]
  mov rcx, rax
  shr rcx, 48
  lea rdx, [g_rings]
  mov rbx, [rdx + rcx * 8]
  mov r12d, eax
  shr rax, 32
  CONN rbp, rbx, r12d
  cmp [rbp + C_GEN], ax
  jne .l
  movzx ecx, word [rbp + C_FLAGS]
  and ecx, F_OPEN | F_CLOSING
  cmp ecx, F_OPEN
  jne .l
  and word [rbp + C_FLAGS], ~F_READY
  test word [rbp + C_FLAGS], F_HELD
  jnz .l                         ; release_held answers it, with everything behind the held write
  dec qword [g_answers]
  mov rdi, rbx
  mov esi, r12d
  xor edx, edx
  xor ecx, ecx
  call serve
  movzx eax, word [rbp + C_FLAGS]
  and eax, F_EOF | F_HELD
  cmp eax, F_EOF
  jne .l
  or word [rbp + C_FLAGS], F_CLOSEAFT
  mov rdi, rbx
  mov esi, r12d
  call mark
  jmp .l
.done:
  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; spill -> eax 1 if some ring accepts afterwards: the accepting ring's table is (nearly) full, so
; accept on the next ring with room, adding one if needed
spill:
  push rbx
  push r12
  sub rsp, 8
  xor r12d, r12d
.any:
  cmp r12, [g_nrings]
  jae .free
  lea rax, [g_rings]
  mov rax, [rax + r12 * 8]
  cmp byte [rax + R_ACCEPTING], 0
  jne .yes
  inc r12
  jmp .any
.free:
  xor r12d, r12d
.f:
  cmp r12, [g_nrings]
  jae .add
  lea rax, [g_rings]
  mov rbx, [rax + r12 * 8]
  cmp byte [rbx + R_FULL], 0
  je .arm
  inc r12
  jmp .f
.add:
  mov rax, [g_nrings]
  cmp rax, MAX_RINGS
  jae .fail
  mov edi, eax
  call ring_new
  test rax, rax
  jnz .added
  ; refused: where a ring's memory counts against RLIMIT_MEMLOCK, the locked code is in its way
  cmp qword [g_locked], 0
  je .fail
  mov qword [g_locked], 0
  call td_unlock_code
  mov edi, [g_nrings]
  call ring_new
  test rax, rax
  jz .fail
.added:
  mov rbx, rax
  mov rcx, [g_nrings]
  lea rdx, [g_rings]
  mov [rdx + rcx * 8], rbx
  inc rcx
  mov [g_nrings], rcx
  mov edi, 2
  lea rsi, [M_RING]
  lea rdx, [rcx - 1]
  xor eax, eax
  call dprintf
.arm:
  mov byte [rbx + R_ACCEPTING], 1
  mov rdi, rbx
  call fill_accepts
.yes:
  mov eax, 1
  jmp .r
.fail:
  mov edi, 2
  lea rsi, [M_NORING]
  mov rdx, [g_nrings]
  xor eax, eax
  call dprintf
  xor eax, eax
.r:
  add rsp, 8
  pop r12
  pop rbx
  ret

; ---- setup ------------------------------------------------------------------------------------

; mmap_anon(rdi = bytes, esi = extra flags) -> rax (or a negative errno)
mmap_anon:
  mov r10d, 0x22                 ; MAP_PRIVATE | MAP_ANONYMOUS
  or r10d, esi
  mov rsi, rdi
  xor edi, edi
  mov edx, 3
  mov r8, -1
  xor r9d, r9d
  mov eax, SYS_mmap
  syscall
  ret

; ring_new(edi = index) -> rax = ring, or 0
ring_new:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 136                   ; io_uring_params (120) and later the registration structs
  mov ebp, edi
  mov edi, 1
  mov esi, R_SIZE
  call calloc
  test rax, rax
  jz .fail
  mov rbx, rax
  mov [rbx + R_INDEX], bp
  mov rdi, rsp
  xor eax, eax
  mov ecx, 136
  rep stosb
  mov dword [rsp + 4], CQ_ENTRIES
  mov dword [rsp + 8], IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN
  mov edi, SQ_ENTRIES
  mov rsi, rsp
  mov eax, SYS_io_uring_setup
  syscall
  test eax, eax
  js .fail
  mov [rbx + R_FD], eax
  mov r15d, eax                  ; fd
  ; one mapping for both rings (IORING_FEAT_SINGLE_MMAP)
  mov eax, [rsp]
  shl eax, 2
  add eax, [rsp + 64]            ; sq_off.array + sq_entries * 4
  mov ecx, [rsp + 4]
  shl ecx, 4
  add ecx, [rsp + 100]           ; cq_off.cqes + cq_entries * 16
  cmp eax, ecx
  cmovb eax, ecx
  mov esi, eax
  xor edi, edi
  mov edx, 3
  mov r10d, 0x8001               ; MAP_SHARED | MAP_POPULATE
  mov r8d, r15d
  xor r9d, r9d
  mov eax, SYS_mmap
  syscall
  cmp rax, -4096
  ja .fail
  mov r12, rax                   ; ring memory
  mov esi, [rsp]
  shl esi, 6
  xor edi, edi
  mov edx, 3
  mov r10d, 0x8001
  mov r8d, r15d
  mov r9d, IORING_OFF_SQES
  mov eax, SYS_mmap
  syscall
  cmp rax, -4096
  ja .fail
  mov [rbx + R_SQES], rax
  mov eax, [rsp + 40]
  add rax, r12
  mov [rbx + R_SQHEAD], rax
  mov eax, [rsp + 44]
  add rax, r12
  mov [rbx + R_SQTAIL], rax
  mov ecx, [rax]
  mov [rbx + R_STAIL], ecx
  mov eax, [rsp + 48]
  mov eax, [r12 + rax]
  mov [rbx + R_SQMASK], eax
  mov eax, [rsp + 52]
  mov eax, [r12 + rax]
  mov [rbx + R_SQENT], eax
  mov eax, [rsp + 80]
  add rax, r12
  mov [rbx + R_CQHEAD], rax
  mov eax, [rsp + 84]
  add rax, r12
  mov [rbx + R_CQTAIL], rax
  mov eax, [rsp + 88]
  mov eax, [r12 + rax]
  mov [rbx + R_CQMASK], eax
  mov eax, [rsp + 100]
  add rax, r12
  mov [rbx + R_CQES], rax
  ; SQ index array: identity
  mov eax, [rsp + 64]
  add rax, r12
  xor ecx, ecx
.arr:
  cmp ecx, [rbx + R_SQENT]
  jae .files
  mov [rax + rcx * 4], ecx
  inc ecx
  jmp .arr
.files:
  ; a sparse table of direct descriptors, as large as the descriptor limit allows
  mov rdi, rsp
  xor eax, eax
  mov ecx, 64
  rep stosb
  mov rax, [g_slots]
  mov [rsp], eax
  mov dword [rsp + 4], IORING_RSRC_REGISTER_SPARSE
  mov edi, r15d
  mov esi, IORING_REGISTER_FILES2
  mov rdx, rsp
  mov r10d, 32
  mov eax, SYS_io_uring_register
  syscall
  test eax, eax
  js .fail
  ; the provided-buffer ring (group 0)
  mov edi, BUF_COUNT * 16
  xor esi, esi
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx + R_BRING], rax
  mov edi, BUF_COUNT * BUF_SIZE
  xor esi, esi
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx + R_BMEM], rax
  mov rdi, rsp
  xor eax, eax
  mov ecx, 64
  rep stosb
  mov rax, [rbx + R_BRING]
  mov [rsp], rax
  mov dword [rsp + 8], BUF_COUNT
  mov r14d, 2
.reg:
  mov edi, r15d
  mov esi, IORING_REGISTER_PBUF_RING
  mov rdx, rsp
  mov r10d, 1
  mov eax, SYS_io_uring_register
  syscall
  test eax, eax
  jz .bufs
  ; Ubuntu 6.8.0-136+ kernels invert the reserved-word check (Launchpad #2162843): they refuse
  ; all zeroes, so a refused registration is retried once with resv[0] = 1.
  cmp eax, -EINVAL
  jne .fail
  mov qword [rsp + 16], 1
  dec r14d
  jnz .reg
  jmp .fail
.bufs:
  xor ecx, ecx
.b:
  mov r8d, ecx
  RECYCLE rbx, r8d
  inc ecx
  cmp ecx, BUF_COUNT
  jb .b
  PUBLISH_BUFS rbx
  ; connection records: one per descriptor slot, zeroed lazily by the kernel
  mov rdi, [g_slots]
  shl rdi, C_SHIFT
  mov esi, 0x4000                ; MAP_NORESERVE
  call mmap_anon
  cmp rax, -4096
  ja .fail
  mov [rbx + R_CONNS], rax
  ; depth: a sixteenth of the table, 8..512, a multiple of 8
  mov rax, [g_slots]
  shr rax, 4
  cmp rax, LISTENERS
  jae .d1
  mov eax, LISTENERS
.d1:
  cmp rax, ACCEPT_DEPTH
  jbe .d2
  mov eax, ACCEPT_DEPTH
.d2:
  and eax, ~(LISTENERS - 1)
  mov [rbx + R_DEPTH], eax
  ; margin: an eighth of the table, between depth + 1 and 1024, at most a quarter of it
  lea ecx, [eax + 1]
  mov rax, [g_slots]
  shr rax, 3
  cmp rax, rcx
  jae .m1
  mov eax, ecx
.m1:
  cmp eax, 2 * ACCEPT_DEPTH
  jbe .m2
  mov eax, 2 * ACCEPT_DEPTH
.m2:
  mov rcx, [g_slots]
  shr rcx, 2
  cmp rax, rcx
  jbe .m3
  mov eax, ecx
.m3:
  mov [rbx + R_MARGIN], eax
  mov rax, rbx
  jmp .r
.fail:
  xor eax, eax
.r:
  add rsp, 136
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

; listener() -> eax = fd or negative errno: SO_REUSEPORT, TCP_NODELAY (inherited by accepted sockets)
listener:
  push rbx
  sub rsp, 32
  mov eax, SYS_socket
  mov edi, 2                     ; AF_INET
  mov esi, 1 | 0x80000           ; SOCK_STREAM | SOCK_CLOEXEC
  xor edx, edx
  syscall
  test eax, eax
  js .r
  mov ebx, eax
  mov dword [rsp + 16], 1
  mov edi, ebx
  mov esi, 1                     ; SOL_SOCKET
  mov edx, 2                     ; SO_REUSEADDR
  lea r10, [rsp + 16]
  mov r8d, 4
  mov eax, SYS_setsockopt
  syscall
  mov edi, ebx
  mov esi, 1
  mov edx, 15                    ; SO_REUSEPORT
  lea r10, [rsp + 16]
  mov r8d, 4
  mov eax, SYS_setsockopt
  syscall
  test eax, eax
  js .r
  mov edi, ebx
  mov esi, 6                     ; IPPROTO_TCP
  mov edx, 1                     ; TCP_NODELAY
  lea r10, [rsp + 16]
  mov r8d, 4
  mov eax, SYS_setsockopt
  syscall
  ; sockaddr_in
  mov qword [rsp], 0
  mov qword [rsp + 8], 0
  mov word [rsp], 2
  mov rax, [g_port]
  xchg al, ah
  mov [rsp + 2], ax
  mov eax, [g_addr]
  mov [rsp + 4], eax
  mov edi, ebx
  mov rsi, rsp
  mov edx, 16
  mov eax, SYS_bind
  syscall
  test eax, eax
  js .r
  mov edi, ebx
  mov esi, 65535
  mov eax, SYS_listen
  syscall
  test eax, eax
  js .r
  mov eax, ebx
.r:
  add rsp, 32
  pop rbx
  ret

; env(rdi = name, rsi = default) -> rax
env:
  push rbx
  mov rbx, rsi
  call getenv
  test rax, rax
  cmovz rax, rbx
  pop rbx
  ret

main:
  push rbx
  push r12
  push r13
  push r14
  push r15
  lea rdi, [ENV_PATH]
  lea rsi, [DEF_PATH]
  call env
  mov r12, rax
  lea rdi, [ENV_SECRET]
  lea rsi, [DEF_SECRET]
  call env
  mov r13, rax
  lea rdi, [ENV_HOST]
  lea rsi, [DEF_HOST]
  call env
  mov [g_host], rax
  lea rdi, [ENV_PORT]
  lea rsi, [DEF_PORT]
  call env
  mov rdi, rax
  call atoi
  movsxd rax, eax
  mov [g_port], rax
  mov edi, 13                    ; SIGPIPE
  mov esi, 1                     ; SIG_IGN
  call signal
  mov edi, 1
  lea rsi, [ts]
  call clock_gettime
  mov rax, [ts]
  mov [g_start_s], rax
  ; the database
  mov rdi, r12
  lea rsi, [errbuf]
  mov edx, 256
  call td_open
  test eax, eax
  jz .db_ok
  mov edi, 2
  lea rsi, [M_DB]
  lea rdx, [errbuf]
  xor eax, eax
  call dprintf
  jmp .die
.db_ok:
  call db_init
  mov rdi, r13
  call strlen
  mov rdi, r13
  mov rsi, rax
  call hmac_init
  ; raise the soft descriptor limit to the hard one: a ring's table may be that large
  mov edi, 7                     ; RLIMIT_NOFILE
  lea rsi, [rl]
  call getrlimit
  mov rax, [rl + 8]
  mov [rl], rax
  mov edi, 7
  lea rsi, [rl]
  call setrlimit
  mov edi, 7
  lea rsi, [rl]
  call getrlimit
  mov rax, [rl]
  mov ecx, 1 << 20
  cmp rax, rcx
  cmova rax, rcx
  mov [g_slots], rax
  ; listen
  mov edi, 2
  mov rsi, [g_host]
  lea rdx, [g_addr]
  call inet_pton
  cmp eax, 1
  je .addr_ok
  mov edi, 2
  lea rsi, [M_HOST]
  mov rdx, [g_host]
  xor eax, eax
  call dprintf
  jmp .die
.addr_ok:
  xor r14d, r14d
.lis:
  call listener
  test eax, eax
  jns .lis_ok
  mov edi, 2
  lea rsi, [M_LISTEN]
  mov rdx, [g_host]
  mov rcx, [g_port]
  mov r8d, eax
  neg r8d
  xor eax, eax
  call dprintf
  jmp .die
.lis_ok:
  lea rcx, [g_listen]
  mov [rcx + r14 * 4], eax
  inc r14d
  cmp r14d, LISTENERS
  jb .lis
  ; connection lists
  mov edi, LIST_CAP * 8
  mov esi, 0x4000
  call mmap_anon
  mov [g_touched], rax
  mov edi, LIST_CAP * 8
  mov esi, 0x4000
  call mmap_anon
  mov [g_rq], rax
  mov edi, LIST_CAP * 8
  mov esi, 0x4000
  call mmap_anon
  mov [g_held], rax
  mov edi, LIST_CAP * 8
  mov esi, 0x4000
  call mmap_anon
  mov [g_ready], rax
  ; ring 0
  xor edi, edi
  call ring_new
  test rax, rax
  jnz .ring_ok
  mov edi, 2
  lea rsi, [M_URING]
  xor eax, eax
  call dprintf
  jmp .die
.ring_ok:
  mov [g_rings], rax
  mov qword [g_nrings], 1
  mov byte [rax + R_ACCEPTING], 1
  mov rdi, rax
  call fill_accepts
  lea rax, [g_wts]
  mov [g_geta + 16], rax
  mov qword [g_wts + 8], WAIT_NS
  call td_lock_code              ; code and read-only data stay in RAM (see shim.c)
  mov [g_locked], rax
  mov edi, 2
  lea rsi, [M_READY]
  mov rdx, [g_host]
  mov rcx, [g_port]
  mov r8, [g_slots]
  mov r9, rax
  xor eax, eax
  call dprintf

.pass:
  ; Submit what the last pass queued and collect completions. Deferred task work only runs
  ; inside an enter with GETEVENTS. The other rings submit and collect without waiting; ring 0
  ; goes last and waits for WAIT_MIN completions or WAIT_NS, whichever comes first, unless
  ; queued reads or answers mean there is work now.
  mov r12, [g_nrings]
.enter:
  dec r12
  js .drain
  lea rax, [g_rings]
  mov rdi, [rax + r12 * 8]
  xor esi, esi
  mov edx, IORING_ENTER_GETEVENTS
  xor ecx, ecx
  test r12, r12
  jnz .go
  mov rax, [g_rqt]
  cmp rax, [g_rqh]
  jne .go
  mov rax, [g_rdt]
  cmp rax, [g_rdh]
  jne .go
  mov esi, WAIT_MIN
  mov edx, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG
  lea rcx, [g_geta]
.go:
  call ring_enter
  jmp .enter
.drain:
  mov qword [g_answers], ANSWERS_PER_PASS
  xor r12d, r12d
.dl:
  cmp r12, [g_nrings]
  jae .reads
  lea rax, [g_rings]
  mov rdi, [rax + r12 * 8]
  call drain
  inc r12
  jmp .dl
.reads:
  call run_ready                 ; oldest first: what earlier passes read, then this pass's reads
  call read_rounds
  cmp qword [g_nheld], 0
  je .end
  cmp qword [g_whold], 0
  jne .end
  call release_held
.end:
  call end_batch
  jmp .pass

.die:
  mov edi, 1
  call exit
