/* Network utilities for HolyC */
#ifndef NET_HC
#define NET_HC

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <errno.h>
#include <pthread.h>

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)

typedef struct {
    U16 sin_family;
    U16 sin_port;
    U32 sin_addr;
    U8 sin_zero[8];
} sockaddr_in;

typedef struct {
    U16 sa_family;
    U8 sa_data[14];
} sockaddr;

typedef struct {
    pthread_mutex_t m;
} Mutex;

#define EnterMutex(m) pthread_mutex_lock(&((m).m))
#define LeaveMutex(m) pthread_mutex_unlock(&((m).m))

#endif
