#define _GNU_SOURCE

#include "uring_rings.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/time_types.h>

#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG (1U << 3)
#endif
#ifndef IORING_REGISTER_PBUF_RING
#define IORING_REGISTER_PBUF_RING 22
#endif
#ifndef IORING_UNREGISTER_PBUF_RING
#define IORING_UNREGISTER_PBUF_RING 23
#endif

#define UR_LISTEN_BACKLOG 4096

static int ur_io_uring_setup(unsigned entries, struct io_uring_params *p) {
  return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int ur_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                             unsigned flags, void *arg, size_t argsz) {
  return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                      arg, argsz);
}

static int ur_io_uring_register(int fd, unsigned opcode, void *arg,
                                unsigned nr_args) {
  return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

uint32_t ur_raise_nofile(void) {
  struct rlimit rl;

  if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
    return 0u;

  if (rl.rlim_cur < rl.rlim_max) {
    struct rlimit raised;
    raised.rlim_cur = rl.rlim_max;
    raised.rlim_max = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &raised) == 0)
      rl.rlim_cur = rl.rlim_max;
  }

  return (uint32_t)rl.rlim_cur;
}

static int ur_make_listener(const struct addrinfo *ai) {
  int fd;
  int one = 1;
  int flags;

  fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
              ai->ai_protocol);
  if (fd < 0) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
      return -1;
  }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  flags = fcntl(fd, F_GETFD, 0);
  if (flags >= 0)
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    close(fd);
    return -1;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    close(fd);
    return -1;
  }
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    close(fd);
    return -1;
  }
  if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, UR_LISTEN_BACKLOG) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

int ur_create_listeners(const char *host, uint16_t port, int *fds,
                        int max_count) {
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *ai;
  char portstr[16];
  char hostbuf[1025];
  const char *node = host;
  int created = 0;

  if (fds == NULL || max_count <= 0)
    return -1;
  if (max_count > MAX_LISTENERS)
    max_count = MAX_LISTENERS;

  if (host != NULL && host[0] == '[') {
    size_t len = strlen(host);
    if (len >= 2 && host[len - 1] == ']' && len - 2 < sizeof(hostbuf)) {
      memcpy(hostbuf, host + 1, len - 2);
      hostbuf[len - 2] = '\0';
      node = hostbuf;
    }
  }
  if (node != NULL && (node[0] == '\0' || strcmp(node, "*") == 0))
    node = NULL;

  snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

  if (getaddrinfo(node, portstr, &hints, &res) != 0)
    return -1;

  for (ai = res; ai != NULL && created < max_count; ai = ai->ai_next) {
    int made_here = 0;

    while (created < max_count) {
      int fd = ur_make_listener(ai);
      if (fd < 0)
        break;
      fds[created++] = fd;
      made_here++;
    }

    if (made_here > 0)
      break;
  }

  freeaddrinfo(res);
  return created > 0 ? created : -1;
}

static void ur_fill_provide_buf(struct io_uring_sqe *sqe, void *addr,
                                uint16_t bid) {
  memset(sqe, 0, sizeof(*sqe));
  sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->fd = -1;
  sqe->off = 1;
  sqe->addr = (uint64_t)(uintptr_t)addr;
  sqe->len = PBUF_SIZE;
  sqe->user_data = (uint64_t)bid;
  sqe->buf_group = PBUF_BGID;
  sqe->addr3 = (uint64_t)bid;
}

static int ur_provide_all_buffers(struct uring_ring *r) {
  uint32_t i;

  for (i = 0; i < PBUF_COUNT; i++) {
    struct io_uring_sqe *sqe = ur_get_sqe(r);

    if (sqe == NULL) {
      if (ur_submit_wait(r, 0, 0) < 0)
        return -1;
      sqe = ur_get_sqe(r);
      if (sqe == NULL)
        return -1;
    }

    ur_fill_provide_buf(sqe, r->pbuf_mem + (size_t)i * PBUF_SIZE,
                        (uint16_t)i);
  }

  if (ur_submit_wait(r, 0, 0) < 0)
    return -1;

  return 0;
}

static int ur_register_pbuf_ring(struct uring_ring *r) {
  struct io_uring_buf_reg reg;
  uint32_t i;
  int ret;

  memset(r->pbuf_ring, 0, r->pbuf_ring_sz);

  for (i = 0; i < PBUF_COUNT; i++) {
    struct io_uring_buf *buf = &r->pbuf_ring->bufs[i];

    buf->addr = (uint64_t)(uintptr_t)(r->pbuf_mem + (size_t)i * PBUF_SIZE);
    buf->len = PBUF_SIZE;
    buf->bid = (uint16_t)i;
    buf->resv = 0;
  }

  r->pbuf_tail_local = (uint16_t)PBUF_COUNT;
  __atomic_store_n(&r->pbuf_ring->tail, r->pbuf_tail_local,
                   __ATOMIC_RELEASE);

  memset(&reg, 0, sizeof(reg));
  reg.ring_addr = (uint64_t)(uintptr_t)r->pbuf_ring;
  reg.ring_entries = PBUF_COUNT;
  reg.bgid = PBUF_BGID;
  reg.flags = 0;

  ret = ur_io_uring_register(r->ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  if (ret < 0 && errno == EINVAL) {

    reg.resv[0] = 1;
    ret = ur_io_uring_register(r->ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  }

  if (ret < 0) {
    if (errno == EINVAL)
      return 1;
    return -1;
  }

  return 0;
}

int ur_ring_init(struct uring_ring *r, uint32_t table_size) {
  static const uint32_t flag_candidates[] = {
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
          IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_SUBMIT_ALL |
          IORING_SETUP_CQSIZE,
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
          IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE,
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
          IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_CQSIZE,
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
          IORING_SETUP_CQSIZE,
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
      IORING_SETUP_SINGLE_ISSUER,
      0u,
  };
  struct io_uring_params p;
  struct io_uring_params init_params;
  size_t sq_ring_sz;
  size_t cq_ring_sz;
  size_t pbuf_mem_sz = (size_t)PBUF_COUNT * PBUF_SIZE;
  size_t pbuf_ring_sz =
      (size_t)PBUF_COUNT * sizeof(struct io_uring_buf);
  int *files = NULL;
  int ret;
  size_t i;
  int saved_errno;

  if (r == NULL)
    return -1;

  memset(r, 0, sizeof(*r));
  r->ring_fd = -1;
  r->table_size = table_size;

  memset(&init_params, 0, sizeof(init_params));
  init_params.cq_entries = RING_CQ_ENTRIES;

  r->ring_fd = -1;
  for (i = 0; i < sizeof(flag_candidates) / sizeof(flag_candidates[0]); i++) {
    p = init_params;
    p.flags = flag_candidates[i];
    r->ring_fd = ur_io_uring_setup(RING_SQ_ENTRIES, &p);
    if (r->ring_fd >= 0)
      break;
    if (errno != EINVAL)
      break;
  }
  if (r->ring_fd < 0)
    return -1;

  sq_ring_sz = (size_t)p.sq_off.array +
               (size_t)p.sq_entries * sizeof(uint32_t);
  cq_ring_sz = (size_t)p.cq_off.cqes +
               (size_t)p.cq_entries * sizeof(struct io_uring_cqe);

  r->sq_ring =
      mmap(NULL, sq_ring_sz, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
  if (r->sq_ring == MAP_FAILED) {
    r->sq_ring = NULL;
    goto fail;
  }
  r->sq_ring_sz = sq_ring_sz;

  r->cq_ring =
      mmap(NULL, cq_ring_sz, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_CQ_RING);
  if (r->cq_ring == MAP_FAILED) {
    r->cq_ring = NULL;
    goto fail;
  }
  r->cq_ring_sz = cq_ring_sz;

  r->sqes = mmap(NULL, (size_t)p.sq_entries * sizeof(struct io_uring_sqe),
                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                 r->ring_fd, IORING_OFF_SQES);
  if (r->sqes == MAP_FAILED) {
    r->sqes = NULL;
    goto fail;
  }

  r->sq_head = (uint32_t *)((uint8_t *)r->sq_ring + p.sq_off.head);
  r->sq_tail = (uint32_t *)((uint8_t *)r->sq_ring + p.sq_off.tail);
  r->sq_mask = (uint32_t *)((uint8_t *)r->sq_ring + p.sq_off.ring_mask);
  r->sq_flags = (uint32_t *)((uint8_t *)r->sq_ring + p.sq_off.flags);
  r->sq_array = (uint32_t *)((uint8_t *)r->sq_ring + p.sq_off.array);

  r->cq_head = (uint32_t *)((uint8_t *)r->cq_ring + p.cq_off.head);
  r->cq_tail = (uint32_t *)((uint8_t *)r->cq_ring + p.cq_off.tail);
  r->cq_mask = (uint32_t *)((uint8_t *)r->cq_ring + p.cq_off.ring_mask);
  r->cqes = (struct io_uring_cqe *)((uint8_t *)r->cq_ring + p.cq_off.cqes);

  r->sq_pending = 0;

  if (table_size > 0) {
    files = malloc((size_t)table_size * sizeof(int));
    if (files == NULL)
      goto fail;
    for (i = 0; i < table_size; i++)
      files[i] = -1;

    if (ur_io_uring_register(r->ring_fd, IORING_REGISTER_FILES, files,
                             table_size) < 0)
      goto fail;
    free(files);
    files = NULL;
  }

  r->pbuf_mem = mmap(NULL, pbuf_mem_sz, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (r->pbuf_mem == MAP_FAILED) {
    r->pbuf_mem = NULL;
    goto fail;
  }

  r->pbuf_ring = mmap(NULL, pbuf_ring_sz, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (r->pbuf_ring == MAP_FAILED) {
    r->pbuf_ring = NULL;
    goto fail;
  }
  r->pbuf_ring_sz = pbuf_ring_sz;

  ret = ur_register_pbuf_ring(r);
  if (ret < 0)
    goto fail;

  if (ret == 0) {
    r->use_pbuf_ring = 1;
    r->avail_pbufs = PBUF_COUNT;
  } else {
    r->use_pbuf_ring = 0;
    r->avail_pbufs = 0;
    if (ur_provide_all_buffers(r) < 0)
      goto fail;
    r->avail_pbufs = PBUF_COUNT;
  }

  r->active = 1;
  return 0;

fail:
  saved_errno = errno;
  free(files);
  ur_ring_destroy(r);
  errno = saved_errno;
  return -1;
}

void ur_ring_destroy(struct uring_ring *r) {
  if (r == NULL)
    return;

  if (r->ring_fd >= 0) {
    if (r->use_pbuf_ring) {
      struct io_uring_buf_reg reg;

      memset(&reg, 0, sizeof(reg));
      reg.bgid = PBUF_BGID;
      (void)ur_io_uring_register(r->ring_fd, IORING_UNREGISTER_PBUF_RING,
                                 &reg, 1);
    }

    if (r->pbuf_ring != NULL)
      munmap(r->pbuf_ring, r->pbuf_ring_sz);
    r->pbuf_ring = NULL;

    if (r->pbuf_mem != NULL)
      munmap(r->pbuf_mem, (size_t)PBUF_COUNT * PBUF_SIZE);
    r->pbuf_mem = NULL;

    if (r->sqes != NULL) {
      size_t sqes_sz = (size_t)RING_SQ_ENTRIES;

      if (r->sq_mask != NULL)
        sqes_sz = (size_t)(*r->sq_mask + 1u);
      munmap(r->sqes, sqes_sz * sizeof(struct io_uring_sqe));
    }
    r->sqes = NULL;

    if (r->sq_ring != NULL)
      munmap(r->sq_ring, r->sq_ring_sz);
    r->sq_ring = NULL;

    if (r->cq_ring != NULL)
      munmap(r->cq_ring, r->cq_ring_sz);
    r->cq_ring = NULL;

    close(r->ring_fd);
  }

  memset(r, 0, sizeof(*r));
  r->ring_fd = -1;
}

struct io_uring_sqe *ur_get_sqe(struct uring_ring *r) {
  uint32_t tail;
  uint32_t mask;
  uint32_t index;
  struct io_uring_sqe *sqe;

  if (r == NULL || r->sq_tail == NULL || r->sq_array == NULL ||
      r->sqes == NULL)
    return NULL;

  mask = *r->sq_mask;
  if (r->sq_pending >= mask + 1)
    return NULL;

  tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
  index = tail & mask;
  sqe = &r->sqes[index];
  memset(sqe, 0, sizeof(*sqe));
  r->sq_array[index] = index;

  __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
  r->sq_pending++;

  return sqe;
}

int ur_submit_wait(struct uring_ring *r, uint32_t wait_nr, int timeout_us) {
  uint32_t to_submit;
  unsigned enter_flags = 0;
  struct io_uring_getevents_arg getevents;
  struct __kernel_timespec ts;
  void *arg = NULL;
  size_t argsz = 0;

  if (r == NULL || r->ring_fd < 0)
    return -1;

  to_submit = r->sq_pending;

  if (wait_nr > 0) {
    enter_flags |= IORING_ENTER_GETEVENTS;

    if (timeout_us > 0) {
      memset(&ts, 0, sizeof(ts));
      ts.tv_sec = timeout_us / 1000000;
      ts.tv_nsec = (long)(timeout_us % 1000000) * 1000L;

      memset(&getevents, 0, sizeof(getevents));
      getevents.sigmask = 0;
      getevents.sigmask_sz = 0;
      getevents.ts = (uint64_t)(uintptr_t)&ts;

      enter_flags |= IORING_ENTER_EXT_ARG;
      arg = &getevents;
      argsz = sizeof(getevents);
    }
  }

  for (;;) {
    int ret = ur_io_uring_enter(r->ring_fd, to_submit, wait_nr, enter_flags,
                                arg, argsz);

    if (ret >= 0) {
      r->sq_pending = 0;
      return ret;
    }

    if (errno != EINTR)
      return -1;

    {
      uint32_t tail = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
      uint32_t head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
      uint32_t outstanding = tail - head;

      if (outstanding > r->sq_pending)
        outstanding = r->sq_pending;
      to_submit = outstanding;
    }
  }
}

void ur_recycle_pbuf(struct uring_ring *r, uint16_t bid) {
  if (r == NULL || bid >= PBUF_COUNT)
    return;

  if (r->use_pbuf_ring) {
    uint16_t tail = r->pbuf_tail_local;
    struct io_uring_buf *buf =
        &r->pbuf_ring->bufs[tail & (uint16_t)(PBUF_COUNT - 1u)];

    buf->addr =
        (uint64_t)(uintptr_t)(r->pbuf_mem + (size_t)bid * PBUF_SIZE);
    buf->len = PBUF_SIZE;
    buf->bid = bid;
    buf->resv = 0;

    tail++;
    __atomic_store_n(&r->pbuf_ring->tail, tail, __ATOMIC_RELEASE);
    r->pbuf_tail_local = tail;
    __atomic_add_fetch(&r->avail_pbufs, 1u, __ATOMIC_RELAXED);
  } else {
    struct io_uring_sqe *sqe = ur_get_sqe(r);

    if (sqe == NULL) {
      if (ur_submit_wait(r, 0, 0) < 0)
        return;
      sqe = ur_get_sqe(r);
      if (sqe == NULL)
        return;
    }

    ur_fill_provide_buf(sqe, r->pbuf_mem + (size_t)bid * PBUF_SIZE, bid);
    __atomic_add_fetch(&r->avail_pbufs, 1u, __ATOMIC_RELAXED);
  }
}

static size_t ur_page_round(size_t n) {
  long page = sysconf(_SC_PAGESIZE);

  if (page <= 0)
    page = 4096;
  if (n == 0)
    n = 1;
  return ((n + (size_t)page - 1u) / (size_t)page) * (size_t)page;
}

struct send_arena *ur_arena_acquire(struct send_arena **pool, size_t min_cap) {
  struct send_arena **link;
  struct send_arena *arena;
  size_t cap;
  size_t alloc;
  void *base;

  if (pool == NULL)
    return NULL;

  link = pool;
  while (*link != NULL) {
    arena = *link;
    if (arena->cap >= min_cap) {
      *link = arena->next;
      arena->next = NULL;
      arena->used = 0;
      __atomic_store_n(&arena->refcnt, 1u, __ATOMIC_RELAXED);
      return arena;
    }
    link = &arena->next;
  }

  cap = min_cap;
  if (cap < ARENA_SIZE)
    cap = ARENA_SIZE;
  alloc = ur_page_round(cap);

  arena = calloc(1, sizeof(*arena));
  if (arena == NULL)
    return NULL;

  base = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    free(arena);
    return NULL;
  }

  arena->base = base;
  arena->used = 0;
  arena->cap = alloc;
  arena->refcnt = 1;
  arena->next = NULL;
  return arena;
}

void ur_arena_release(struct send_arena **pool, struct send_arena *a) {
  uint32_t refs;

  if (a == NULL)
    return;

  refs = __atomic_load_n(&a->refcnt, __ATOMIC_ACQUIRE);
  if (refs > 1) {
    __atomic_store_n(&a->refcnt, refs - 1u, __ATOMIC_RELEASE);
    return;
  }

  a->used = 0;
  __atomic_store_n(&a->refcnt, 0u, __ATOMIC_RELEASE);
  a->next = NULL;

  if (pool != NULL) {
    a->next = *pool;
    *pool = a;
  }
}

void ur_arena_free_pool(struct send_arena *pool) {
  while (pool != NULL) {
    struct send_arena *next = pool->next;

    if (pool->base != NULL && pool->cap > 0)
      munmap(pool->base, pool->cap);
    free(pool);
    pool = next;
  }
}
