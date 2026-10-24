; io_uring rings: setup, submission queue entries, submitting, provided buffers.
;
; Every client socket lives in a ring's fixed-file table instead of the process fd table, so the
; 65,535-fd limit applies per ring, not per process. A ring holds up to [slots] sockets; more rings
; are created as earlier ones fill up (MAXRINGS in all). Ring 0 uses DEFER_TASKRUN: its
; completions are posted only when this thread enters the kernel for them, in one batch, and the
; loop waits inside io_uring_enter on it (an eventfd wakeup was measured to arrive milliseconds
; late with DEFER_TASKRUN). The other rings don't: their completions are posted as task work on
; the way back from any syscall, so they need an io_uring_enter only to submit. Ring 0 polls their
; fds (multishot), which wakes the wait when one of them gets completions.

%define SYS_mmap              9
%define SYS_eventfd2          290
%define SYS_prlimit64         302
%define SYS_io_uring_setup    425
%define SYS_io_uring_enter    426
%define SYS_io_uring_register 427

; io_uring_params
%define P_SQ_ENTRIES      0
%define P_CQ_ENTRIES      4
%define P_FLAGS           8
%define P_FEATURES        20
%define P_SQ_HEAD         40
%define P_SQ_TAIL         44
%define P_SQ_MASK         48
%define P_SQ_ENTRIES_OFF  52
%define P_SQ_FLAGS        56
%define P_SQ_ARRAY        64
%define P_CQ_HEAD         80
%define P_CQ_TAIL         84
%define P_CQ_MASK         88
%define P_CQ_OVERFLOW     96
%define P_CQ_CQES         100

%define SETUP_CQSIZE          (1 << 3)
%define SETUP_SUBMIT_ALL      (1 << 7)
%define SETUP_COOP_TASKRUN    (1 << 8)
%define SETUP_TASKRUN_FLAG    (1 << 9)
%define SETUP_SINGLE_ISSUER   (1 << 12)
%define SETUP_DEFER_TASKRUN   (1 << 13)
%define SETUP_NO_SQARRAY      (1 << 16)
%define RING_FLAGS (SETUP_CQSIZE | SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN | SETUP_TASKRUN_FLAG | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN)
%define RING_FLAGS_K (SETUP_CQSIZE | SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN | SETUP_SINGLE_ISSUER)   ; rings 1..

%define ENTER_GETEVENTS       1
%define ENTER_EXT_ARG         8
%define ENTER_REGISTERED_RING 16
%define SQ_CQ_OVERFLOW        2              ; sq flags: completions wait in the kernel's overflow list
%define SQ_TASKRUN            4              ; sq flags: completions are waiting to be posted

%define REG_FILES2            13
%define REG_RING_FDS          20
%define REG_PBUF_RING         22

; opcodes, sqe flags
%define OPC_POLL_ADD      6
%define OPC_ACCEPT        13
%define OPC_ASYNC_CANCEL  14
%define OPC_CLOSE         19
%define OPC_SEND          26
%define OPC_SHUTDOWN      34
%define OPC_PROVIDE_BUFFERS 31
%define OPC_RECV          27
%define IOSQE_FIXED_FILE      1
%define IOSQE_IO_LINK         4
%define IOSQE_IO_HARDLINK     8
%define IOSQE_BUFFER_SELECT   32
%define IOSQE_CQE_SKIP_SUCCESS 64
%define ACCEPT_MULTISHOT      1              ; sqe ioprio
%define RECV_MULTISHOT        2              ; sqe ioprio
%define CANCEL_ALL            1
%define CANCEL_FD             2
%define CANCEL_FD_FIXED       8
%define FILE_INDEX_ALLOC      0xFFFFFFFF
%define CQE_F_BUFFER          1
%define CQE_F_MORE            2
%define MSG_DONTWAIT          0x40
%define POLLOUT               4
%define POLLIN                1
%define POLL_ADD_MULTI        1

; sqe fields
%define SQE_OPCODE        0
%define SQE_FLAGS         1
%define SQE_IOPRIO        2
%define SQE_FD            4
%define SQE_OFF           8
%define SQE_ADDR          16
%define SQE_LEN           24
%define SQE_OPFLAGS       28
%define SQE_USER_DATA     32
%define SQE_BUF_GROUP     40
%define SQE_FILE_INDEX    44

%ifndef BATCH_N
%define BATCH_N           32             ; a busy loop waits for this many completions ...
%endif
%ifndef BATCH_US
%define BATCH_US          250            ; ... or this many microseconds, whichever comes first
%endif

; sizes
%define MAXRINGS          8
%define SQ_ENTRIES        4096
%define CQ_ENTRIES        32768
%define NBUF              2048           ; provided receive buffers per ring (4 MB; 512 ran out in connect bursts)
%define BUF_SHIFT         10             ; 1 KB: requests here are smaller; a longer one spans buffers
%define BUF_SZ            (1 << BUF_SHIFT)

; ring record (128 bytes)
%define RG_FD             0
%define RG_EFD            4              ; fd argument for io_uring_enter (registered index or fd)
%define RG_SQHEAD         8
%define RG_SQTAIL         16
%define RG_SQES           24
%define RG_CQHEAD         32
%define RG_CQTAIL         40
%define RG_CQES           48
%define RG_SQFLAGS        56
%define RG_BUFRING        64
%define RG_BUFBASE        72
%define RG_SQMASK         80
%define RG_CQMASK         84
%define RG_LTAIL          88             ; local sq tail (sqes written)
%define RG_BTAIL          96             ; provided-buffer ring tail
%define RG_NOPEN          100            ; open connections in this ring's file table
%define RG_EFLAGS         104            ; extra io_uring_enter flags (ENTER_REGISTERED_RING or 0)
%define RG_CQOVF          112            ; kernel counter of dropped completions
%define RG_LEGACY         120            ; 1: buffers are given back with PROVIDE_BUFFERS (no buffer ring)
%define RG_SHIFT          7

section .bss
alignb 64
rings:      resb MAXRINGS << RG_SHIFT
uparams:    resb 128
ureg:       resb 64
nrings:     resd 1
slots:      resd 1                ; fixed-file slots per ring
rlim:       resq 2
ts_idle:    resq 2                ; idle wait with more than one ring (their completions can't wake
                                  ; a wait on ring 0, so look at their task-run flags this often)
ts_batch:   resq 2                ; how long a busy loop waits to collect a batch
wait_arg:   resq 3                ; io_uring_getevents_arg

section .rodata
msg_uring:  db `io_uring setup failed (needs Linux 6.1+)\n`, 0

section .text

; RING r, idx: r = address of ring idx's record
%macro RING 2
  mov %1, %2
  shl %1, RG_SHIFT
  lea r11, [rings]
  add %1, r11
%endmacro

; uring_init: raise the fd soft limit, size the fixed-file tables, create the eventfd
uring_init:
  push rbx
  ; prlimit64(0, RLIMIT_NOFILE, NULL, &rlim) then set soft = hard
  xor edi, edi
  mov esi, 7
  xor edx, edx
  lea r10, [rlim]
  SYSCALL SYS_prlimit64
  mov rax, [rlim+8]
  mov [rlim], rax
  xor edi, edi
  mov esi, 7
  lea rdx, [rlim]
  xor r10d, r10d
  SYSCALL SYS_prlimit64
  mov rax, [rlim]
  cmp rax, 65535
  jbe .lim
  mov eax, 65535
.lim:
  sub eax, 16                 ; the kernel's check is nr <= RLIMIT_NOFILE; keep a little slack
  mov [slots], eax
  mov qword [ts_idle+8], 1000000         ; 1 ms
  mov qword [ts_batch+8], BATCH_US * 1000
  pop rbx
  ret

uring_fail:
  lea rdi, [msg_uring]
  call puts2
  mov edi, 1
  call exit

; ring_create -> eax = index of the new ring (fatal on failure)
ring_create:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r12d, [nrings]
  RING rbx, r12
  mov r15d, RING_FLAGS
  test r12d, r12d
  jz .flags
  mov r15d, RING_FLAGS_K
.flags:
  ; io_uring_setup
  lea rdi, [uparams]
  xor eax, eax
  mov ecx, 16
  rep stosq
  mov dword [uparams+P_CQ_ENTRIES], CQ_ENTRIES
  mov [uparams+P_FLAGS], r15d
  or dword [uparams+P_FLAGS], SETUP_NO_SQARRAY
  mov edi, SQ_ENTRIES
  lea rsi, [uparams]
  SYSCALL SYS_io_uring_setup
  test eax, eax
  jns .ok
  ; kernels before 6.6 have no NO_SQARRAY: use an identity sq array instead
  lea rdi, [uparams]
  xor eax, eax
  mov ecx, 16
  rep stosq
  mov dword [uparams+P_CQ_ENTRIES], CQ_ENTRIES
  mov [uparams+P_FLAGS], r15d
  mov edi, SQ_ENTRIES
  lea rsi, [uparams]
  SYSCALL SYS_io_uring_setup
  test eax, eax
  js uring_fail
.ok:
  mov [rbx+RG_FD], eax
  mov [rbx+RG_EFD], eax
  mov r13d, eax
  ; one mapping for both rings: max(sq array end, cqes end)
  mov eax, [uparams+P_SQ_ARRAY]
  mov ecx, [uparams+P_SQ_ENTRIES]
  lea r14, [rax + rcx*4]
  mov eax, [uparams+P_CQ_CQES]
  mov ecx, [uparams+P_CQ_ENTRIES]
  shl rcx, 4
  add rax, rcx
  cmp rax, r14
  cmova r14, rax
  xor edi, edi
  mov rsi, r14
  mov edx, 3                  ; PROT_READ | PROT_WRITE
  mov r10d, 0x8001            ; MAP_SHARED | MAP_POPULATE
  mov r8d, r13d
  xor r9d, r9d                ; IORING_OFF_SQ_RING
  SYSCALL SYS_mmap
  cmp rax, -4096
  ja uring_fail
  mov r15, rax                ; ring base
  mov eax, [uparams+P_SQ_HEAD]
  add rax, r15
  mov [rbx+RG_SQHEAD], rax
  mov eax, [uparams+P_SQ_TAIL]
  add rax, r15
  mov [rbx+RG_SQTAIL], rax
  mov eax, [uparams+P_SQ_FLAGS]
  add rax, r15
  mov [rbx+RG_SQFLAGS], rax
  mov eax, [uparams+P_CQ_HEAD]
  add rax, r15
  mov [rbx+RG_CQHEAD], rax
  mov eax, [uparams+P_CQ_TAIL]
  add rax, r15
  mov [rbx+RG_CQTAIL], rax
  mov eax, [uparams+P_CQ_CQES]
  add rax, r15
  mov [rbx+RG_CQES], rax
  mov eax, [uparams+P_CQ_OVERFLOW]
  add rax, r15
  mov [rbx+RG_CQOVF], rax
  mov eax, [uparams+P_SQ_MASK]
  mov eax, [r15+rax]
  mov [rbx+RG_SQMASK], eax
  mov eax, [uparams+P_CQ_MASK]
  mov eax, [r15+rax]
  mov [rbx+RG_CQMASK], eax
  ; identity sq array, when there is one
  test dword [uparams+P_FLAGS], SETUP_NO_SQARRAY
  jnz .noarr
  mov eax, [uparams+P_SQ_ARRAY]
  lea rdi, [r15+rax]
  xor ecx, ecx
.arr:
  mov [rdi+rcx*4], ecx
  inc ecx
  cmp ecx, [uparams+P_SQ_ENTRIES]
  jb .arr
.noarr:
  ; sqes
  xor edi, edi
  mov esi, [uparams+P_SQ_ENTRIES]
  shl rsi, 6
  mov edx, 3
  mov r10d, 0x8001
  mov r8d, r13d
  mov r9d, 0x10000000         ; IORING_OFF_SQES
  SYSCALL SYS_mmap
  cmp rax, -4096
  ja uring_fail
  mov [rbx+RG_SQES], rax
  ; sparse fixed-file table
  lea rdi, [ureg]
  xor eax, eax
  mov ecx, 8
  rep stosq
  mov eax, [slots]
  mov [ureg], eax             ; nr
  mov dword [ureg+4], 1       ; IORING_RSRC_REGISTER_SPARSE
  mov edi, r13d
  mov esi, REG_FILES2
  lea rdx, [ureg]
  mov r10d, 32
  SYSCALL SYS_io_uring_register
  test eax, eax
  js uring_fail
  ; provided receive buffers: the ring of buffer entries, then the buffers (plus a guard page,
  ; so a parser that peeks a few bytes past the last buffer stays inside the mapping)
  xor edi, edi
  mov esi, NBUF*16 + NBUF*BUF_SZ + 4096
  mov edx, 3
  mov r10d, 0x22              ; MAP_PRIVATE | MAP_ANONYMOUS
  mov r8, -1
  xor r9d, r9d
  SYSCALL SYS_mmap
  cmp rax, -4096
  ja uring_fail
  mov [rbx+RG_BUFRING], rax
  lea rcx, [rax + NBUF*16]
  mov [rbx+RG_BUFBASE], rcx
  lea rdi, [ureg]
  xor eax, eax
  mov ecx, 8
  rep stosq
  mov rax, [rbx+RG_BUFRING]
  mov [ureg], rax             ; ring_addr
  mov dword [ureg+8], NBUF    ; ring_entries, bgid 0
  mov edi, r13d
  mov esi, REG_PBUF_RING
  lea rdx, [ureg]
  mov r10d, 1
%ifdef FORCE_LEGACY
  mov eax, -22                ; (test build: behave as if buffer rings were refused)
%else
  SYSCALL SYS_io_uring_register
%endif
  test eax, eax
  jns .pbuf
  ; some kernels refuse buffer rings (Ubuntu's 6.8.0-147 answers EINVAL): fall back to the older
  ; provided buffers, handed to the kernel with an sqe (all of them now, then one per reuse)
  mov dword [rbx+RG_LEGACY], 1
  mov rdi, rbx
  call get_sqe
  mov byte [rax+SQE_OPCODE], OPC_PROVIDE_BUFFERS
  mov dword [rax+SQE_FD], NBUF          ; number of buffers
  mov rcx, [rbx+RG_BUFBASE]
  mov [rax+SQE_ADDR], rcx
  mov dword [rax+SQE_LEN], BUF_SZ
  mov qword [rax+SQE_USER_DATA], OP_CANCEL
  jmp .bufs_done
.pbuf:
  mov rdi, [rbx+RG_BUFRING]
  mov rsi, [rbx+RG_BUFBASE]
  xor ecx, ecx
.buf:
  mov rax, rcx
  shl rax, BUF_SHIFT
  add rax, rsi
  mov [rdi], rax              ; addr
  mov dword [rdi+8], BUF_SZ   ; len
  mov [rdi+12], cx            ; bid (bytes 14-15 of entry 0 are the ring tail)
  add rdi, 16
  inc ecx
  cmp ecx, NBUF
  jb .buf
  mov dword [rbx+RG_BTAIL], NBUF
  mov rdi, [rbx+RG_BUFRING]
  mov word [rdi+14], NBUF
.bufs_done:
  ; register the ring fd, so io_uring_enter skips the fd lookup (optional)
  lea rdi, [ureg]
  xor eax, eax
  mov ecx, 8
  rep stosq
  mov dword [ureg], -1        ; offset: any free slot
  mov [ureg+8], r13           ; data: the ring fd
  mov edi, r13d
  mov esi, REG_RING_FDS
  lea rdx, [ureg]
  mov r10d, 1
  SYSCALL SYS_io_uring_register
  cmp eax, 1
  jne .noreg
  mov eax, [ureg]
  mov [rbx+RG_EFD], eax
  mov dword [rbx+RG_EFLAGS], ENTER_REGISTERED_RING
.noreg:
  test r12d, r12d
  jz .first
  mov edi, r12d
  call arm_rpoll
.first:
  inc dword [nrings]
  mov eax, r12d
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  ret

; ring_enter(rdi = ring record): hand written sqes to the kernel and post waiting completions
; Clobbers rax, rcx, rdx, rsi, r8-r11. Keeps rdi.
ring_enter:
  mov eax, [rdi+RG_LTAIL]
  mov rcx, [rdi+RG_SQTAIL]
  mov [rcx], eax              ; publish (x86 stores are ordered)
  mov rcx, [rdi+RG_SQHEAD]
  mov esi, eax
  sub esi, [rcx]              ; to_submit: everything the kernel hasn't consumed yet
  push rdi
  xor edx, edx                ; min_complete
  mov r10d, ENTER_GETEVENTS
  or r10d, [rdi+RG_EFLAGS]
  xor r8d, r8d
  xor r9d, r9d
  mov edi, [rdi+RG_EFD]
  SYSCALL SYS_io_uring_enter
  pop rdi
  ret

; ring_submit(rdi = ring): hand written sqes to the kernel, without posting completions (so a
; pass that fills a submission queue doesn't keep finding new work and never get to its flush)
; Clobbers rax, rcx, rdx, rsi, r8-r11. Keeps rdi.
ring_submit:
  mov eax, [rdi+RG_LTAIL]
  mov rcx, [rdi+RG_SQTAIL]
  mov [rcx], eax
  mov rcx, [rdi+RG_SQHEAD]
  mov esi, eax
  sub esi, [rcx]
  push rdi
  xor edx, edx
  mov r10d, [rdi+RG_EFLAGS]
  xor r8d, r8d
  xor r9d, r9d
  mov edi, [rdi+RG_EFD]
  SYSCALL SYS_io_uring_enter
  pop rdi
  ret

; ring_needs_enter(rdi = ring) -> ZF=0 if there are unsubmitted sqes or completions to post
ring_needs_enter:
  mov eax, [rdi+RG_LTAIL]
  mov rcx, [rdi+RG_SQHEAD]
  cmp eax, [rcx]
  jne .yes
  mov rax, [rdi+RG_SQFLAGS]
  lea rcx, [rings]
  cmp rdi, rcx
  jne .other
  test dword [rax], SQ_TASKRUN | SQ_CQ_OVERFLOW
  ret
.other:                       ; no deferred task work on the other rings
  test dword [rax], SQ_CQ_OVERFLOW
  ret
.yes:
  or eax, 1                   ; ZF=0
  ret

; arm_rpoll(edi = ring k > 0): a multishot poll on ring 0 for ring k's fd. It completes (on ring 0,
; which the loop waits on) whenever ring k has completions. Keeps rbx, r12-r15.
arm_rpoll:
  mov ecx, edi
  RING rdx, rcx
  push rdx
  push rcx
  RING rdi, 0
  call get_sqe
  pop rcx
  pop rdx
  mov byte [rax+SQE_OPCODE], OPC_POLL_ADD
  mov ecx, ecx
  shl ecx, 8
  or ecx, OP_RPOLL
  mov [rax+SQE_USER_DATA], rcx
  mov ecx, [rdx+RG_FD]
  mov [rax+SQE_FD], ecx
  mov dword [rax+SQE_LEN], POLL_ADD_MULTI
  mov dword [rax+SQE_OPFLAGS], POLLIN
  ret

; enter_all(esi = 1: every ring): io_uring_enter every ring that has sqes or completions waiting.
; The kernel (seen on 6.8 and 7.2) can leave SQ_TASKRUN set on a ring with no work queued, and
; the loop would spin entering it. So when an enter posts nothing, the flag is cleared here.
; That can race with work arriving right then (whose flag would be lost), so the loop also
; enters every ring once a millisecond.
enter_all:
  push rbx
  push r12
  xor r12d, r12d
  mov ebx, esi
.l:
  cmp r12d, [nrings]
  jae .done
  RING rdi, r12
  inc r12d
  test ebx, ebx
  jnz .enter
  call ring_needs_enter
  jz .l
.enter:
  call ring_enter
  mov rax, [rdi+RG_CQHEAD]
  mov eax, [rax]
  mov rcx, [rdi+RG_CQTAIL]
  cmp eax, [rcx]
  jne .l                      ; completions came: the flag means something
  mov rax, [rdi+RG_SQFLAGS]
  lock and dword [rax], ~SQ_TASKRUN
  jmp .l
.done:
  pop r12
  pop rbx
  ret

; get_sqe(rdi = ring) -> rax = a zeroed sqe. Keeps rdi; clobbers rax, rcx, rdx, rsi, r8-r11.
get_sqe:
  mov eax, [rdi+RG_LTAIL]
  mov rcx, [rdi+RG_SQHEAD]
  mov ecx, [rcx]
  mov edx, eax
  sub edx, ecx
  cmp edx, SQ_ENTRIES
  jb .room
  call ring_submit            ; the queue is full: submit what is there (and nothing else)
  jmp get_sqe
.room:
  inc dword [rdi+RG_LTAIL]
  and eax, [rdi+RG_SQMASK]
  shl rax, 6
  add rax, [rdi+RG_SQES]
  pxor xmm0, xmm0
  movdqu [rax], xmm0
  movdqu [rax+16], xmm0
  movdqu [rax+32], xmm0
  movdqu [rax+48], xmm0
  ret

; put_buf(rdi = ring, esi = buffer id): give a receive buffer back to the kernel
; Clobbers rax, rcx, rdx, rsi, r8-r11.
put_buf:
  cmp dword [rdi+RG_LEGACY], 0
  jne .legacy
  mov eax, [rdi+RG_BTAIL]
  mov ecx, eax
  and ecx, NBUF - 1
  shl rcx, 4
  add rcx, [rdi+RG_BUFRING]
  mov edx, esi
  shl rdx, BUF_SHIFT
  add rdx, [rdi+RG_BUFBASE]
  mov [rcx], rdx
  mov dword [rcx+8], BUF_SZ
  mov [rcx+12], si
  inc eax
  mov [rdi+RG_BTAIL], eax
  mov rcx, [rdi+RG_BUFRING]
  mov [rcx+14], ax            ; new tail (u16), after the entry is written
  ret
.legacy:
  push rsi
  call get_sqe
  pop rsi
  mov byte [rax+SQE_OPCODE], OPC_PROVIDE_BUFFERS
  mov byte [rax+SQE_FLAGS], IOSQE_CQE_SKIP_SUCCESS
  mov dword [rax+SQE_FD], 1
  mov edx, esi
  shl rdx, BUF_SHIFT
  add rdx, [rdi+RG_BUFBASE]
  mov [rax+SQE_ADDR], rdx
  mov dword [rax+SQE_LEN], BUF_SZ
  mov [rax+SQE_OFF], esi      ; buffer id
  mov qword [rax+SQE_USER_DATA], OP_CANCEL
  ret

; ring_wait(esi = min_complete, rdx = timeout (__kernel_timespec) or 0): submit ring 0's queued
; sqes and sleep until it has min_complete completions or the timeout passes.
ring_wait:
  push rbx
  mov ebx, esi
  RING rdi, 0
  mov eax, [rdi+RG_LTAIL]
  mov rcx, [rdi+RG_SQTAIL]
  mov [rcx], eax
  mov rcx, [rdi+RG_SQHEAD]
  sub eax, [rcx]
  mov esi, eax                ; to_submit
  mov r10d, ENTER_GETEVENTS
  or r10d, [rdi+RG_EFLAGS]
  xor r8d, r8d
  xor r9d, r9d
  test rdx, rdx
  jz .go
  mov [wait_arg+16], rdx
  or r10d, ENTER_EXT_ARG
  lea r8, [wait_arg]
  mov r9d, 24
.go:
  mov edx, ebx
  mov edi, [rdi+RG_EFD]
  SYSCALL SYS_io_uring_enter
  pop rbx
  ret
