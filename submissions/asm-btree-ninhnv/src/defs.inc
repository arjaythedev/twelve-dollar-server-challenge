; Shared constants, structure layouts and macros. Every .asm file includes this.
; All functions follow the System V AMD64 ABI (args rdi, rsi, rdx, rcx, r8, r9; result rax;
; rbx, rbp, r12-r15 preserved; rsp 16-byte aligned at every call).
default rel

; ---- syscalls -------------------------------------------------------------------------------
%define SYS_close         3
%define SYS_mmap          9
%define SYS_socket        41
%define SYS_bind          49
%define SYS_listen        50
%define SYS_setsockopt    54
%define SYS_getrlimit     97
%define SYS_setrlimit     160
%define SYS_io_uring_setup    425
%define SYS_io_uring_enter    426
%define SYS_io_uring_register 427

%define EINTR    4
%define EAGAIN   11
%define ENFILE   23
%define EBUSY    16
%define ENOBUFS  105
%define ETIME    62
%define EINVAL   22

; ---- io_uring -------------------------------------------------------------------------------
%define IORING_SETUP_CQSIZE        (1 << 3)
%define IORING_SETUP_SUBMIT_ALL    (1 << 7)
%define IORING_SETUP_SINGLE_ISSUER (1 << 12)
%define IORING_SETUP_DEFER_TASKRUN (1 << 13)
%define IORING_ENTER_GETEVENTS     1
%define IORING_ENTER_EXT_ARG       8
%define IORING_OFF_SQES            0x10000000
%define IORING_REGISTER_FILES2     13
%define IORING_RSRC_REGISTER_SPARSE 1
%define IORING_REGISTER_PBUF_RING  22
%define IORING_FILE_INDEX_ALLOC    0xffffffff
%define IORING_RECV_MULTISHOT      2
%define IORING_CQE_F_BUFFER        1
%define IORING_CQE_F_MORE          2

%define OP_CLOSE    19
%define OP_ACCEPT   13
%define OP_SEND     26
%define OP_RECV     27
%define OP_SHUTDOWN 34

%define SQE_FIXED_FILE    1
%define SQE_IO_HARDLINK   8
%define SQE_BUFFER_SELECT 32

; SQE field offsets (64 bytes)
%define SQE_OPCODE   0
%define SQE_FLAGS    1
%define SQE_IOPRIO   2
%define SQE_FD       4
%define SQE_OFF      8
%define SQE_ADDR     16
%define SQE_LEN      24
%define SQE_OPFLAGS  28
%define SQE_USERDATA 32
%define SQE_BUFGROUP 40
%define SQE_FILEIDX  44

; CQE (16 bytes)
%define CQE_USERDATA 0
%define CQE_RES      8
%define CQE_FLAGS    12

%define MSG_DONTWAIT 0x40
%define MSG_NOSIGNAL 0x4000

; completion tags: op << 56 | ring << 48 | gen << 32 | slot
%define T_ACCEPT   1
%define T_RECV     2
%define T_SEND     3
%define T_SHUTDOWN 4
%define T_CLOSE    5
%define T_READ     6

; ---- sizes and limits -----------------------------------------------------------------------
%define SQ_ENTRIES   4096
%define CQ_ENTRIES   65536
%define BUF_SIZE     2048          ; receive buffer
%define BUF_COUNT    1024          ; receive buffers per ring (power of two)
%define LISTENERS    8
%define ACCEPT_DEPTH 512
%define MAX_RINGS    16
%define WAIT_MIN     256
%define WAIT_NS      125000
%define READS_PER_PASS 16384
%define ANSWERS_PER_PASS 2048
%define LIST_CAP     (1 << 21)     ; entries in each connection list (ring buffers, u64 each)
%define OUT_CAP      16376         ; data bytes of a pooled output buffer (16 KB with header)
%define POOL_MAX     2048
%define MAX_HEAD     16384
%define MAX_BODY     (1024 * 1024)
%define FEED_LEN     20
%define RANGE_SPAN   512

; ---- ring record ----------------------------------------------------------------------------
%define R_FD        0
%define R_PENDING   4
%define R_SQHEAD    8
%define R_SQTAIL    16
%define R_SQMASK    24
%define R_SQENT     28
%define R_SQES      32
%define R_CQHEAD    40
%define R_CQTAIL    48
%define R_CQMASK    56
%define R_STAIL     60    ; local SQ tail
%define R_CQES      64
%define R_BRING     72
%define R_BMEM      80
%define R_BTAIL     88    ; u32 (low 16 bits used)
%define R_ACCEPTING 92    ; byte
%define R_FULL      93    ; byte
%define R_INDEX     94    ; u16
%define R_CONNS     96
%define R_OPEN      104
%define R_MARGIN    108
%define R_DEPTH     112
%define R_READS     116
%define R_ACC       120   ; u32 x LISTENERS
%define R_SIZE      (120 + 4 * LISTENERS)

; ---- connection record (64 bytes) -----------------------------------------------------------
%define C_GEN   0     ; u16
%define C_FLAGS 2     ; u16
%define C_SENT  4     ; u32
%define C_OUT   8     ; Buf*: responses not yet handed to the kernel
%define C_WIRE  16    ; Buf*: the send in flight
%define C_PART  24    ; Buf*: bytes not yet answered (an incomplete or held request)
%define C_MARK  32    ; u32: C_OUT length when this pass first touched it
%define C_SHIFT 6     ; log2(record size)

%define F_OPEN     1
%define F_CLOSEAFT 2     ; close once the output drains
%define F_HELD     4     ; a write waits out a checkpoint hold
%define F_EOF      8     ; the client is done sending (while held)
%define F_CLOSING  16
%define F_SENDING  32
%define F_QUEUED   64    ; on the read queue
%define F_RECVING  128   ; a single-shot read is in flight
%define F_INBATCH  256   ; on the touched list
%define F_ANSWERED 512   ; produced a response in this pass
%define F_READY    1024  ; on the ready list: read bytes wait in C_PART for a later pass

; ---- byte buffers: [0] u32 len, [4] u32 cap, data from 8 ---------------------------------------
%define B_LEN  0
%define B_CAP  4
%define B_DATA 8

; ---- parsed request -------------------------------------------------------------------------
%define Q_METHOD  0    ; 0 GET, 1 POST, 2 other
%define Q_PATH    8
%define Q_PLEN    16
%define Q_AUTH    24   ; 0 if absent
%define Q_ALEN    32
%define Q_BODY    40
%define Q_BLEN    48
%define Q_USED    56
%define Q_SIZE    64

; ---- JSON member slots (json_obj) -----------------------------------------------------------
%define J_TYPE  0      ; 0 absent, 1 string, 2 number, 3 other
%define J_START 8
%define J_LEN   16
%define J_ESC   24     ; string contains escapes
%define J_SIZE  32

; ---- macros ---------------------------------------------------------------------------------

; Copies a constant string (padded with 16 bytes in .rodata, see LIT) to [rdi] in 16-byte chunks
; and advances rdi. Writes up to 15 bytes past the string: every output buffer keeps slack.
%macro WLIT 1
  %assign %%n %1_len
  %assign %%o 0
  %rep (%%n + 15) / 16
    movdqu xmm0, [%1 + %%o]
    movdqu [rdi + %%o], xmm0
    %assign %%o %%o + 16
  %endrep
  add rdi, %%n
%endmacro

; Defines a string constant `name` with `name_len`, padded for WLIT.
%macro LIT 2
  %1: db %2
  %1_len equ $ - %1
  times 16 db 0
%endmacro

section .note.GNU-stack noalloc noexec nowrite progbits
