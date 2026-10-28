/* Network utilities for HolyC */
#ifndef NET_HC
#define NET_HC

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9

/* sockaddr / socket types */
typedef struct {
    I16 sin_family;
    I16 sin_port;
    U32 sin_addr;
    U8 sin_zero[8];
} sockaddr_in;

typedef struct {
    I16 sa_family;
    U8 sa_data[14];
} sockaddr;

#ifndef I32
#define I32 I32
#endif
#ifndef I64
#define I64 I64
#endif
#ifndef U8
#define U8 U8
#endif
#ifndef U16
#define U16 U16
#endif
#ifndef U32
#define U32 U32
#endif

/* Basic socket operations - declarations only */
I32 socket(I32 domain, I32 type, I32 protocol);
I32 bind(I32 s, U8 *addr, I32 addrlen);
I32 listen(I32 s, I32 backlog);
I32 setsockopt(I32 s, I32 level, I32 optname, U8 *optval, I32 optlen);
I32 setsockopt(I32 s, I32 level, I32 optname, U8 *optval, I32 optlen);
I32 fcntl(I32 fd, I32 cmd, I32 arg);
I32 close(I32 fd);
I32 send(I32 fd, U8 *buf, I32 len, I32 flags);
I32 recv(I32 fd, U8 *buf, I32 len, I32 flags);
I32 accept(I32 listen_sock, U8 *addr, I32 *addrlen);
I32 select(I32 nfds, U8 *readfds, U8 *writefds, U8 *exceptfds, struct timeval *timeout);

struct timeval {
    I32 tv_sec;
    I32 tv_usec;
} ;

/* Mutex support */
typedef struct {
    U8 slot[24]; /* opaque pthread_mutex_t placeholder */
} Mutex;

#define EnterMutex(m) ((void)0)
#define LeaveMutex(m) ((void)0)

#endif