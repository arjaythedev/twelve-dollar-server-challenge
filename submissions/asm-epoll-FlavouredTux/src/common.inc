; common.inc - Definitions and constants for Twelve Dollar Server Challenge (x86_64 ASM)

%ifndef COMMON_INC
%define COMMON_INC

; SQLite constants
%define SQLITE_OK               0
%define SQLITE_ERROR            1
%define SQLITE_ROW              100
%define SQLITE_DONE             101
%define SQLITE_OPEN_READWRITE   2
%define SQLITE_STATIC           0
%define SQLITE_TRANSIENT        -1

; Socket & Epoll constants
%define AF_INET                 2
%define SOCK_STREAM             1
%define SOCK_NONBLOCK           0x800
%define SOCK_CLOEXEC            0x80000

%define SOL_SOCKET              1
%define SO_REUSEADDR            2
%define SO_REUSEPORT            15
%define IPPROTO_TCP             6
%define TCP_NODELAY             1

%define EPOLLIN                 0x001
%define EPOLLOUT                0x004
%define EPOLLERR                0x008
%define EPOLLHUP                0x010
%define EPOLLRDHUP              0x2000
%define EPOLLET                 0x80000000

%define EPOLL_CTL_ADD           1
%define EPOLL_CTL_DEL           2
%define EPOLL_CTL_MOD           3
%define EPOLL_CLOEXEC           0x80000

; Limits & Buffer sizes
%define MAX_EVENTS              4096
%define MAX_FDS                 65536
%define REQ_BUF_SIZE            8192
%define RESP_BUF_SIZE           65536

; Connection state structure (per file descriptor)
struc conn_state
    .fd         resd 1
    .pad        resd 1
    .read_len   resq 1
    .read_buf   resb REQ_BUF_SIZE
endstruc

; Parsed JWT info structure
struc jwt_info
    .user_id        resq 1
    .username_len   resq 1
    .username       resb 256
endstruc

%endif
