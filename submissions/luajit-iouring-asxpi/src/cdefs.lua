-- Every C declaration the server uses: libc, the io_uring ABI and the SQLite shim.
local ffi = require("ffi")

ffi.cdef [[
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
void *memchr(const void *, int, size_t);
void *memmove(void *, const void *, size_t);
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
char *strerror(int);
int close(int);
int usleep(unsigned);
ssize_t write(int, const void *, size_t);

typedef struct { int64_t sec; int64_t nsec; } td_timespec;
int clock_gettime(int, td_timespec *);

typedef struct { uint64_t cur, max; } td_rlimit;
int getrlimit(int, td_rlimit *);
int setrlimit(int, const td_rlimit *);

typedef struct { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; } td_sockaddr_in;
int socket(int, int, int);
int setsockopt(int, int, int, const void *, uint32_t);
int bind(int, const td_sockaddr_in *, uint32_t);
int listen(int, int);
int inet_pton(int, const char *, void *);
uint16_t htons(uint16_t);
void *mmap(void *, size_t, int, int, int, long);
typedef void (*td_sighandler)(int);
td_sighandler signal(int, td_sighandler);

/* io_uring ABI. user_data is split in two 32-bit halves so Lua never handles a 64-bit integer. */
typedef struct {
  uint8_t opcode, flags; uint16_t ioprio; int32_t fd;
  uint64_t off;
  uint8_t *addr;
  uint32_t len; uint32_t op_flags;
  uint32_t ud_lo, ud_hi;
  uint16_t buf_group, personality; uint32_t file_index;
  uint64_t addr3, pad2;
} td_sqe;
typedef struct { uint32_t ud_lo, ud_hi; int32_t res; uint32_t flags; } td_cqe;
typedef struct { uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1; uint64_t user_addr; } td_sqoff;
typedef struct { uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1; uint64_t user_addr; } td_cqoff;
typedef struct {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd, resv[3];
  td_sqoff sq_off; td_cqoff cq_off;
} td_uring_params;
typedef struct { uint32_t nr, flags; uint64_t resv2; uint64_t data; uint64_t tags; } td_rsrc_register;
typedef struct { uint8_t *ring_addr; uint32_t ring_entries; uint16_t bgid, flags; uint64_t resv[3]; } td_buf_reg;
typedef struct { uint8_t *addr; uint32_t len; uint16_t bid; uint16_t tail; } td_buf;
typedef struct { uint64_t sigmask; uint32_t sigmask_sz, min_wait_usec; td_timespec *ts; } td_getevents_arg;

int td_sys_setup(long nr, unsigned entries, td_uring_params *p) __asm__("syscall");
int td_sys_enter(long nr, unsigned fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                 void *arg, size_t argsz) __asm__("syscall");
int td_sys_register(long nr, unsigned fd, unsigned opcode, void *arg, unsigned nr_args) __asm__("syscall");

/* one connection: 40 bytes, idle connections own no buffers */
typedef struct {
  uint32_t gen;
  uint32_t flags;
  uint32_t inlen, incap;
  uint32_t outlen, outsent, outcap;
  uint8_t *inb;
  uint8_t *outb;
} td_conn;

/* shim.c */
const char *td_errmsg(void);
int td_open(const char *path, int cache_kb, int autockpt);
int td_begin(void);
int td_commit(void);
int td_row(int cur, int64_t rowid, uint8_t *buf, uint32_t cap);
int td_exists(int cur, int64_t rowid);
int td_first(int cur, uint8_t *buf, uint32_t cap);
int td_next(int cur, uint8_t *buf, uint32_t cap);
int td_count_likes(int64_t post_id);
int td_prev(int cur, uint8_t *buf, uint32_t cap);
int64_t td_rowid(int cur);
int td_likes_seek(int64_t post_id, uint8_t *buf, uint32_t cap);
int td_insert_post(int64_t user_id, const uint8_t *body, int n, int64_t *id, uint8_t *created, int created_cap);
int td_insert_like(int64_t user_id, int64_t post_id);
int td_health(void);
int td_user_row(int k, int64_t rowid, uint8_t *buf, uint32_t cap);
int td_post_row(int k, int64_t rowid, uint8_t *buf, uint32_t cap);
int td_json_clean(const uint8_t *, int);
int td_wal_frames(void);
int td_ckpt_start(void);
int td_ckpt_boost(int on);
int td_ckpt_running(void);
int td_ckpt_left(void);
int td_ckpt_rounds(void);
int td_ckpt_done(void);
]]

return ffi.C
