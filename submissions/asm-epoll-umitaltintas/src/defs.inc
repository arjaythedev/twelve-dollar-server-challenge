; Constants, syscall numbers and small macros shared by every part of the server.

%define SYS_read          0
%define SYS_write         1
%define SYS_close         3
%define SYS_socket        41
%define SYS_bind          49
%define SYS_listen        50
%define SYS_setsockopt    54

%define EAGAIN            11
%define ENOBUFS           105
%define ENFILE            23
%define ECANCELED         125

%define SOCK_NONBLOCK     0x800
%define MSG_NOSIGNAL      0x4000
%define MSG_MORE          0x8000

%define SQLITE_ROW        100
%define SQLITE_DONE       101

; Sizes
%define MAXCONN           (8 << 16)      ; connection id = ring << 16 | fixed-file slot (MAXRINGS rings)
%define CONN_SHIFT        6              ; 64-byte connection records
%define DQ_CAP            (1 << 20)      ; set-aside completions (power of two)
%define ACC_K             256            ; single-shot accepts in flight (enough for connect bursts)
%define ACC_RESERVE       1024           ; move accepting to the next ring this close to a full table
%define ACC_LOW           4096           ; a ring takes accepts again once this many slots are free
%define SCRATCH_SZ        65536          ; a request split over several receives is joined here
%define MAX_REQ           32768          ; bigger requests are rejected (bodies > 16 KB are out of scope)
%define BODY_SZ           131072         ; one response body is built here
%define ARENA_SZ          (8*1024*1024)  ; all responses of one batch, sent after the commit
%define ARENA_SLACK       (256*1024)     ; flush early when less than this is left
%define FEED_WIN          4096           ; feed: widest id range counted with one likes scan
%define PASS_MAX          2048           ; completions handled per pass before the responses go out
%define MAXREC            4096           ; response records per batch
%define CKPT_MAIN_FRAMES  4096           ; the event loop checkpoints when the WAL reaches this size (16 MB)

; Completion kinds (low byte of an sqe's user_data)
%define OP_ACCEPT         1
%define OP_RECV           2
%define OP_SEND           3
%define OP_POLL           4
%define OP_CANCEL         5
%define OP_CLOSE          6
%define OP_RESUME         7              ; not from the kernel: handle input held while output drained
%define OP_RPOLL          8              ; ring 0's poll on another ring's fd

; Connection record (64 bytes, indexed by connection id)
%define C_INBUF           0              ; leftover partial request (malloc) or 0
%define C_INLEN           8
%define C_FLAGS           12
%define C_OUTBUF          16             ; output the socket could not take yet (malloc) or 0
%define C_OUTLEN          24
%define C_OUTOFF          28
%define C_GEN             32             ; generation (16 bits): bumped on close, in every user_data
%define C_SLEN            36             ; the send in flight: length and data
%define C_SPTR            40
%define F_ALIVE           1
%define F_CLOSE_AFTER     2              ; close once the output is sent
%define F_SENDING         4              ; a send is submitted and its result not seen yet
%define F_SEND_OUT        8              ; ... and it sends C_OUTBUF (not the arena)
%define F_POLLING         16             ; waiting for the socket to become writable
%define F_WRITABLE        32             ; became writable: send C_OUTBUF at the next flush
%define F_CLOSING         64             ; a "Connection: close" response is queued

; Response record (16 bytes): fd, arena offset, length, flags
%define R_FD              0
%define R_START           4
%define R_LEN             8
%define R_FLAGS           12
%define RF_WRITE          1              ; depends on the batch transaction committing
%define RF_CLOSE          2              ; close the connection after sending

; Status line indexes (status_tab)
%define ST_200            0
%define ST_201            1
%define ST_400            2
%define ST_401            3
%define ST_404            4
%define ST_500            5
%define ST_503            6

; W_LIT "text": append a literal at rdi (advances rdi; clobbers rsi, rcx)
%macro W_LIT 1
  [section .rodata]
  %%s: db %1
  %%e:
  __?SECT?__
  lea rsi, [%%s]
  mov ecx, %%e - %%s
  rep movsb
%endmacro

; REPLY status, "body": build the literal body and queue the response
%macro REPLY 2
  lea rdi, [bodybuf]
  W_LIT %2
  mov rdx, rdi
  mov ecx, %1
  call emit
%endmacro

; SYSCALL nr: raw Linux syscall (args already in rdi, rsi, rdx, r10, r8, r9)
%macro SYSCALL 1
  mov eax, %1
  syscall
%endmacro
