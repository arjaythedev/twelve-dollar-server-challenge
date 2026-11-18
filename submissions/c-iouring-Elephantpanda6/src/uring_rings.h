#ifndef URING_RINGS_H
#define URING_RINGS_H

#include <linux/io_uring.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_RINGS 16
#define MAX_LISTENERS 8
#define RING_SQ_ENTRIES 4096u
#define RING_CQ_ENTRIES 16384u
#define PBUF_COUNT 1024u
#define PBUF_SIZE 2048u
#define PBUF_BGID 1u
#define ARENA_SIZE (512u * 1024u)

struct send_arena {
  uint8_t *base;
  size_t used;
  size_t cap;
  uint32_t refcnt;
  struct send_arena *next;
};

struct uring_ring {
  int ring_fd;
  int active;
  int use_pbuf_ring;
  uint32_t table_size;
  uint32_t active_conns;

  void *sq_ring;
  size_t sq_ring_sz;
  void *cq_ring;
  size_t cq_ring_sz;
  struct io_uring_sqe *sqes;

  uint32_t *sq_head;
  uint32_t *sq_tail;
  uint32_t *sq_mask;
  uint32_t *sq_flags;
  uint32_t *sq_array;
  uint32_t sq_pending;

  uint32_t *cq_head;
  uint32_t *cq_tail;
  uint32_t *cq_mask;
  struct io_uring_cqe *cqes;

  uint8_t *pbuf_mem;
  struct io_uring_buf_ring *pbuf_ring;
  size_t pbuf_ring_sz;
  uint16_t pbuf_tail_local;
  uint32_t avail_pbufs;
};

uint32_t ur_raise_nofile(void);

int ur_create_listeners(const char *host, uint16_t port, int *fds, int max_count);

int ur_ring_init(struct uring_ring *r, uint32_t table_size);
void ur_ring_destroy(struct uring_ring *r);

struct io_uring_sqe *ur_get_sqe(struct uring_ring *r);

int ur_submit_wait(struct uring_ring *r, uint32_t wait_nr, int timeout_us);

void ur_recycle_pbuf(struct uring_ring *r, uint16_t bid);

struct send_arena *ur_arena_acquire(struct send_arena **pool, size_t min_cap);
void ur_arena_release(struct send_arena **pool, struct send_arena *a);
void ur_arena_free_pool(struct send_arena *pool);

#endif
