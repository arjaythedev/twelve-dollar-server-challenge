; Constants, syscall numbers and small macros shared by every part of the server.

%define SYS_read          0
%define SYS_write         1
%define SYS_close         3
%define SYS_socket        41
%define SYS_accept4       288
%define SYS_sendto        44
%define SYS_bind          49
%define SYS_listen        50
%define SYS_setsockopt    54
%define SYS_epoll_wait    232
%define SYS_epoll_ctl     233
%define SYS_epoll_create1 291

%define EAGAIN            11
%define EINTR             4
%define ENFILE            23
%define EMFILE            24

%define EPOLLIN           0x001
%define EPOLLOUT          0x004
%define EPOLL_CTL_ADD     1
%define EPOLL_CTL_MOD     3

%define SOCK_NONBLOCK     0x800
%define MSG_NOSIGNAL      0x4000
%define MSG_MORE          0x8000

%define SQLITE_ROW        100
%define SQLITE_DONE       101

; Sizes
%define MAXCONN           65536          ; fds are below LimitNOFILE=65535
%ifndef KA_PER_IP
%define KA_PER_IP         20000          ; keep-alive budget per client address (bucket)
%endif
%define IP_BUCKETS        65536          ; client addresses are counted in hashed buckets
%ifndef KA_LIMIT
%define KA_LIMIT          60000          ; above this many open connections, stop keeping them alive
%endif
%define CONN_SHIFT        6              ; 64-byte connection records
%define MAXEV             1024           ; epoll events per batch
%define SCRATCH_SZ        65536          ; one request is read and parsed here
%define MAX_REQ           32768          ; bigger requests are rejected (bodies > 16 KB are out of scope)
%define BODY_SZ           131072         ; one response body is built here
%define ARENA_SZ          (8*1024*1024)  ; all responses of one batch, sent after the commit
%define ARENA_SLACK       (256*1024)     ; flush early when less than this is left
%define MAXREC            4096           ; response records per batch
%define CKPT_BG_FRAMES    128            ; background checkpoint when the WAL has this many frames
%define CKPT_MAIN_FRAMES  4096           ; the event loop completes the checkpoint at this size (16 MB)

; Connection record (64 bytes, indexed by fd)
%define C_INBUF           0              ; leftover partial request (malloc) or 0
%define C_INLEN           8
%define C_FLAGS           12             ; F_ALIVE, F_CLOSE_AFTER, F_NOKA
%define C_OUTBUF          16             ; unsent output (malloc) or 0
%define C_OUTLEN          24
%define C_OUTOFF          28
%define C_IPB             32             ; client-address bucket (index into ipcnt)
%define F_ALIVE           1
%define F_CLOSE_AFTER     2
%define F_NOKA            4              ; accepted over KA_LIMIT: no keep-alive

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
