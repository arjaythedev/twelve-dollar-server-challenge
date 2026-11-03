\ ============================================================================
\ ffi.fs - shared C library bindings for the whole project (sqlite3 + crypto).
\ This is the ONE c-library block. Both the SQLite layer and the JWT layer
\ pull their C functions from here.
\ ============================================================================

c-library tdsc

\c #include <stdint.h>
\c #include <stddef.h>

\ --- sqlite3 prototypes ---
\c extern int sqlite3_open_v2(const char*, void**, int, const char*);
\c extern int sqlite3_close_v2(void*);
\c extern int sqlite3_exec(void*, const char*, void*, void*, char**);
\c extern int sqlite3_prepare_v2(void*, const char*, int, void**, const char**);
\c extern int sqlite3_step(void*);
\c extern int sqlite3_reset(void*);
\c extern int sqlite3_finalize(void*);
\c extern int sqlite3_clear_bindings(void*);
\c extern int sqlite3_bind_int64(void*, int, int64_t);
\c extern int sqlite3_bind_text(void*, int, const char*, int, void*);
\c extern int64_t sqlite3_column_int64(void*, int);
\c extern const unsigned char *sqlite3_column_text(void*, int);
\c extern int sqlite3_column_bytes(void*, int);
\c extern const char *sqlite3_errmsg(void*);
\c extern int64_t sqlite3_last_insert_rowid(void*);
\c extern int sqlite3_changes(void*);
\c extern int sqlite3_wal_checkpoint_v2(void*, const char*, int, int*, int*);

\ --- libcrypto prototypes (used by jwt.fs) ---
\c extern unsigned char *HMAC(const void *evp_md, const void *key, int key_len,
\c     const unsigned char *d, size_t n, unsigned char *md, unsigned int *md_len);
\c extern const void *EVP_sha256(void);

\ --- sockets (used by http.fs) ---
\c #include <sys/types.h>
\c #include <sys/socket.h>
\c #include <netinet/in.h>
\c #include <netinet/tcp.h>
\c #include <arpa/inet.h>
\c #include <unistd.h>
\c #include <errno.h>
\c #include <signal.h>
\c #include <string.h>
\c
\c /* Create a listening TCP socket bound to host:port with SO_REUSEADDR.
\c    host is a dotted-quad string (e.g. "127.0.0.1") or "0.0.0.0". Returns
\c    the listening fd, or -1 on error. */
\c static int tdsc_listen(const char *host, int port, int backlog) {
\c   int fd = socket(AF_INET, SOCK_STREAM, 0);
\c   if (fd < 0) return -1;
\c   int one = 1;
\c   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
\c   struct sockaddr_in a;
\c   memset(&a, 0, sizeof(a));
\c   a.sin_family = AF_INET;
\c   a.sin_port = htons((unsigned short)port);
\c   if (host == 0 || host[0] == 0) a.sin_addr.s_addr = htonl(INADDR_ANY);
\c   else if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
\c   if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
\c   if (listen(fd, backlog) != 0) { close(fd); return -1; }
\c   return fd;
\c }
\c static int tdsc_close(int fd) { return close(fd); }
\c static int tdsc_set_nodelay(int fd) {
\c   int one = 1; return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
\c }
\c static void tdsc_ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }
\c
\c /* ---- epoll event loop support ---- */
\c #include <sys/epoll.h>
\c #include <fcntl.h>
\c #define TDSC_MAXEV 4096
\c static struct epoll_event tdsc_evbuf[TDSC_MAXEV];
\c static int tdsc_epoll_create(void) { return epoll_create1(0); }
\c /* set O_NONBLOCK on fd */
\c static int tdsc_set_nonblock(int fd) {
\c   int fl = fcntl(fd, F_GETFL, 0); if (fl < 0) return -1;
\c   return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
\c }
\c /* non-blocking accept: returns fd, or -1 (check errno EAGAIN) */
\c static int tdsc_accept_nb(int lfd) {
\c   int fd = accept(lfd, 0, 0);
\c   if (fd >= 0) { int fl = fcntl(fd, F_GETFL, 0); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
\c   return fd;
\c }
\c /* epoll_ctl add/mod/del. op: 1=ADD 2=DEL 3=MOD. events bitmask (EPOLLIN=1,EPOLLOUT=4). */
\c static int tdsc_epoll_ctl(int epfd, int op, int fd, unsigned int events) {
\c   int realop = (op==1)?EPOLL_CTL_ADD:(op==2)?EPOLL_CTL_DEL:EPOLL_CTL_MOD;
\c   struct epoll_event ev; ev.events = events; ev.data.fd = fd;
\c   return epoll_ctl(epfd, realop, fd, &ev);
\c }
\c /* wait; fills internal tdsc_evbuf. returns number of ready events (or -1). */
\c static int tdsc_epoll_wait(int epfd, int timeout_ms) {
\c   int n; do { n = epoll_wait(epfd, tdsc_evbuf, TDSC_MAXEV, timeout_ms); }
\c   while (n < 0 && errno == EINTR); return n;
\c }
\c static int tdsc_ev_fd(int i)     { return tdsc_evbuf[i].data.fd; }
\c static unsigned int tdsc_ev_ev(int i) { return tdsc_evbuf[i].events; }
\c /* non-blocking recv/send: return bytes, 0 on EOF, -1 on EAGAIN/error (check errno) */
\c static long tdsc_recv_nb(int fd, char *buf, long n) {
\c   long r = recv(fd, buf, (size_t)n, 0); return r;
\c }
\c static long tdsc_send_nb(int fd, const char *buf, long n) {
\c   long w = send(fd, buf, (size_t)n, MSG_NOSIGNAL); return w;
\c }
\c static int tdsc_errno(void) { return errno; }

s" sqlite3" add-lib
s" crypto"  add-lib

\ sqlite3 ( args -- ret )
c-function sqlite3_open_v2      sqlite3_open_v2      a a n a -- n
c-function sqlite3_close_v2     sqlite3_close_v2     a -- n
c-function sqlite3_exec         sqlite3_exec         a a a a a -- n
c-function sqlite3_prepare_v2   sqlite3_prepare_v2   a a n a a -- n
c-function sqlite3_step         sqlite3_step         a -- n
c-function sqlite3_reset        sqlite3_reset        a -- n
c-function sqlite3_finalize     sqlite3_finalize     a -- n
c-function sqlite3_clear_bindings sqlite3_clear_bindings a -- n
c-function sqlite3_bind_int64   sqlite3_bind_int64   a n n -- n
c-function sqlite3_bind_text    sqlite3_bind_text    a n a n a -- n
c-function sqlite3_column_int64 sqlite3_column_int64 a n -- n
c-function sqlite3_column_text  sqlite3_column_text  a n -- a
c-function sqlite3_column_bytes sqlite3_column_bytes a n -- n
c-function sqlite3_errmsg       sqlite3_errmsg       a -- a
c-function sqlite3_last_insert_rowid sqlite3_last_insert_rowid a -- n
c-function sqlite3_changes      sqlite3_changes      a -- n
c-function sqlite3_wal_checkpoint_v2 sqlite3_wal_checkpoint_v2 a a n a a -- n

\ crypto
c-function HMAC        HMAC        a a n a n a a -- a
c-function EVP_sha256  EVP_sha256  -- a

\ sockets (custom C helpers above)
c-function tdsc_listen        tdsc_listen       a n n -- n
c-function tdsc_close         tdsc_close        n -- n
c-function tdsc_set_nodelay   tdsc_set_nodelay  n -- n
c-function tdsc_ignore_sigpipe tdsc_ignore_sigpipe -- void

\ epoll + non-blocking I/O
c-function tdsc_epoll_create  tdsc_epoll_create  -- n
c-function tdsc_set_nonblock  tdsc_set_nonblock  n -- n
c-function tdsc_accept_nb     tdsc_accept_nb     n -- n
c-function tdsc_epoll_ctl     tdsc_epoll_ctl     n n n n -- n
c-function tdsc_epoll_wait    tdsc_epoll_wait    n n -- n
c-function tdsc_ev_fd         tdsc_ev_fd         n -- n
c-function tdsc_ev_ev         tdsc_ev_ev         n -- n
c-function tdsc_recv_nb       tdsc_recv_nb       n a n -- n
c-function tdsc_send_nb       tdsc_send_nb       n a n -- n
c-function tdsc_errno         tdsc_errno         -- n

end-c-library
