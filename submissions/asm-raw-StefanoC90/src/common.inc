; Shared constants and structure layouts for the feed server.
; NASM include file, no code.

; ---- Linux syscalls (x86-64) ----
%define SYS_read            0
%define SYS_write           1
%define SYS_close           3
%define SYS_writev          20
%define SYS_socket          41
%define SYS_accept          43
%define SYS_bind            49
%define SYS_listen          50
%define SYS_setsockopt      54
%define SYS_clock_gettime   228

%define CLOCK_REALTIME      0
%define CLOCK_MONOTONIC     1

%define AF_INET             2
%define SOCK_STREAM         1
%define SOL_SOCKET          1
%define SO_REUSEADDR        2
%define SO_RCVTIMEO         20
%define IPPROTO_TCP         6
%define TCP_NODELAY         1

%define EINTR               -4
%define EAGAIN              -11

; ---- HTTP ----
%define REQ_SIZE            65536
%define RESP_SIZE           262144
%define TMP_SIZE            32768
%define USER_SIZE           16384
%define HEADER_RESERVE      512

%define M_OTHER             0
%define M_GET               1
%define M_POST              2

%define ST_200              0
%define ST_201              1
%define ST_400              2
%define ST_401              3
%define ST_404              4
%define ST_500              5
%define ST_503              6

; parse_request output
%define P_METHOD            0
%define P_PATH              8
%define P_PATHLEN           16
%define P_VERSION11         24
%define P_CLOSE             28
%define P_AUTH              32
%define P_AUTHLEN           40
%define P_BODYLEN           48
%define P_BODYOFF           56
%define P_HEADEREND         64
%define P_SIZE              72

; worker context (one per thread)
%define W_DB                0
%define W_REQ               8
%define W_RESP              16
%define W_TMP               24
%define W_USER              32
%define W_STMT_FEED         40
%define W_STMT_GET          48
%define W_STMT_INS          56
%define W_STMT_EXISTS       64
%define W_STMT_LIKE         72
%define W_STMT_HEALTH       80
%define W_SIZE              88

; one rendered post
%define R_ID                0
%define R_BODY              8
%define R_BODYLEN           16
%define R_CREATED           24
%define R_CREATEDLEN        32
%define R_AUTHOR            40
%define R_AUTHORLEN         48
%define R_LIKES             56
%define R_SIZE              64

; db_create_post argument block
%define C_UID               0
%define C_BODY              8
%define C_BODYLEN           16
%define C_UNAME             24
%define C_UNAMELEN          32
%define C_DST               40
%define C_OUTLEN            48
%define C_SIZE              56

; JWT payload parse result
%define J_UID               0
%define J_UNAMELEN          8
%define J_EXP               16
%define J_FLAGS             24
%define J_SUBLEN            32
%define J_SIZE              40

%define JF_EXP              1
%define JF_SUB              2
%define JF_UNAME            4

; SQLite constants
%define SQLITE_OK           0
%define SQLITE_ROW          100
%define SQLITE_DONE         101
%define SQLITE_OPEN_RW      6          ; READWRITE | CREATE
%define SQLITE_TRANSIENT    -1
