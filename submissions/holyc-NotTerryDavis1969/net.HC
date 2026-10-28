/* Network utilities for HolyC */
#ifndef NET_HC
#define NET_HC

typedef struct {
    I32 sin_family;
    I32 sin_port;
    U64 sin_addr;
    U8 sin_zero[8];
} sockaddr_in;

typedef struct {
    I32 sa_family;
    U8 sa_data[14];
} sockaddr;

typedef struct {
    U16 sa_family;
    U8 sa_data[14];
} sockaddr;

I32 socket(I32 domain, I32 type, I32 protocol);
I32 accept(I32 listen_sock, U8 *addr, I32 addrlen);
I32 bind(I32 s, U8 *addr, I32 addrlen);
I32 listen(I32 s, I32 backlog);
I32 setsockopt(I32 s, I32 level, I32 optname, U8 *optval, I32 optlen);
I32 setsockopt(I32 s, I32 level, I32 optname, U8 *optval, I32 optlen);
I32 fcntl(I32 sock, I32 cmd, I32 flags);
I32 close(I32 fd);
I32 send(I32 fd, U8 *buf, I32 len, I32 flags);
I32 recv(I32 fd, U8 *buf, I32 len, I32 flags);
I32 select(I32 max_fds, U8 *read_fds, I8 *write_fds, I8 *except_fds, struct timeval *timeout);

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define O_NONBLOCK 04000

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)

typedef struct {
    I16 sin_family;
    U16 sin_port;
    U32 sin_addr;
    U8 sin_zero[8];
} sockaddr_in;

typedef struct {
    U16 sa_family;
    U8 sa_data[14];
} sockaddr;

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)

typedef struct {
    pthread_mutex_t m;
} Mutex;

#define EnterMutex(m) pthread_mutex_lock(&((m).m))
#define LeaveMutex(m) pthread_mutex_unlock(&((m).m))

#endif