/* MIT. Linux and buffer adapters. Policy lives in transport.goose.
 * io_uring calls follow rust-iouring-kensac and goose-iouring-ahmadsh2007.
 * Included through c_headers, so the generated C can inline short adapters. */
#ifndef ANTHKNEE_IO_H
#define ANTHKNEE_IO_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <liburing.h>
#include <malloc.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "goose.h"

#define IO_RINGS 16
#define IO_RX 2048
#define IO_BYTES 1024
#define IO_BOX_FIRST 256
#define IO_BOX_LIMIT (256u << 20)
#define IO_BOX_MAX (2u << 20)
#define IO_BOX_POOL_LIMIT 1024
#define IO_BOX_POOL_MAX 32768
#define IO_BOX_CLASSES 8
#define IO_INPUT_NATIVE_LIMIT (6u << 20)
#define IO_INPUT_NATIVE_FIRST 512
#define IO_TRIM_DROP (16u << 20)
#define IO_TRIM_INTERVAL_US 5000000

typedef struct {
  struct io_uring queue;
  struct io_uring_buf_ring *buffers;
  uint8_t *bytes;
  unsigned returned;
} io_ring;
typedef struct { uint32_t length, capacity; uint8_t bytes[]; } io_box;
static io_ring *io_rings[IO_RINGS];
static int io_ring_count;
static int io_listeners[8];
static io_box **io_boxes;
static int32_t *io_free;
static int32_t io_box_count, io_box_capacity, io_free_count;
static uint64_t io_box_memory, io_box_pool_count, io_box_pool_bytes, io_box_highwater;
static uint64_t io_input_memory;
static uint64_t io_input_native_memory;
static int64_t io_trim_at;
/* Empty handles and retained allocations have separate lists. Pool heads and
 * idle boxes' length fields store the next handle plus one; zero ends a list. */
static int32_t io_pool[IO_BOX_CLASSES];
static int32_t io_input_pool[IO_BOX_CLASSES];

/* Goose's overflow pool and all native buffers share the capacity guard.
 * The pool reports its backing high-water length, including reusable holes. */
static inline uint8_t io_input_reserve(int64_t capacity) {
  if (capacity < 0 || (uint64_t)capacity > IO_BOX_LIMIT ||
      io_box_memory + (uint64_t)capacity > IO_BOX_LIMIT) return false;
  io_input_memory = (uint64_t)capacity;
  return true;
}

static inline unsigned io_box_class(uint32_t capacity) {
  unsigned bin = 0;
  while (capacity > IO_BOX_FIRST) { capacity /= 2; bin++; }
  return bin;
}

static inline void io_box_grew(void) {
  if (io_box_memory > io_box_highwater) io_box_highwater = io_box_memory;
}

/* A net capacity drop, not cumulative allocation churn, triggers reclamation.
 * Re-arm at the current capacity even when libc cannot release a whole page. */
static inline uint8_t io_trim_due(int64_t now) {
  if (io_box_highwater - io_box_memory < IO_TRIM_DROP ||
      now - io_trim_at < IO_TRIM_INTERVAL_US) return false;
  io_trim_at = now;
  io_box_highwater = io_box_memory;
  return true;
}
static inline uint8_t io_heap_trim(void) { return malloc_trim(0) != 0; }

/* Known-size payloads allocate their final capacity before the first copy.
 * malloc/realloc leave bytes uninitialized, as alloc_copy does for a Goose pool. */
static inline int32_t io_box_new_from(int64_t needed, bool input) {
  if (needed < 0 || needed > IO_BOX_MAX) return -1;
  uint32_t capacity = input ? IO_INPUT_NATIVE_FIRST : IO_BOX_FIRST;
  while (capacity < (uint64_t)needed) capacity *= 2;
  unsigned bin = io_box_class(capacity);
  int32_t *pool = input ? io_input_pool : io_pool;
  if (bin < IO_BOX_CLASSES && pool[bin]) {
    int32_t handle = pool[bin] - 1;
    io_box *box = io_boxes[handle];
    assert(box && box->capacity == capacity);
    pool[bin] = (int32_t)box->length;
    io_box_pool_count--; io_box_pool_bytes -= capacity;
    box->length = 0;
    return handle;
  }
  /* Retained input buffers still count against the native fast-path budget. */
  if (input && io_input_native_memory + capacity > IO_INPUT_NATIVE_LIMIT) return -1;
  int32_t handle;
  if (io_free_count) {
    handle = io_free[--io_free_count];
    assert(io_boxes[handle] == NULL);
  } else {
    if (io_box_count >= 3 * IO_RINGS * 65535) return -1;
    if (io_box_count == io_box_capacity) {
      int32_t capacity = io_box_capacity ? io_box_capacity * 2 : 4096;
      io_box **boxes = realloc(io_boxes, (size_t)capacity * sizeof *boxes);
      if (!boxes) return -1;
      io_boxes = boxes;
      int32_t *handles = realloc(io_free, (size_t)capacity * sizeof *handles);
      if (!handles) return -1;
      io_free = handles;
      io_box_capacity = capacity;
    }
    handle = io_box_count++;
    io_boxes[handle] = NULL;
  }
  if (io_box_memory + io_input_memory + capacity > IO_BOX_LIMIT) { io_free[io_free_count++] = handle; return -1; }
  io_box *box = malloc(sizeof *box + capacity);
  if (!box) { io_free[io_free_count++] = handle; return -1; }
  box->length = 0; box->capacity = capacity;
  io_box_memory += capacity;
  if (input) io_input_native_memory += capacity;
  io_box_grew();
  io_boxes[handle] = box;
  return handle;
}
static inline int32_t io_box_new(int64_t needed) { return io_box_new_from(needed, false); }
static inline int32_t io_input_new(int64_t needed) { return io_box_new_from(needed, true); }

static inline void io_box_drop_from(int32_t handle, bool input) {
  assert(handle >= 0 && handle < io_box_count);
  io_box *box = io_boxes[handle];
  assert(box != NULL);
  if (io_box_pool_count >= IO_BOX_POOL_LIMIT || box->capacity > IO_BOX_POOL_MAX) {
    io_box_memory -= box->capacity;
    if (input) io_input_native_memory -= box->capacity;
    free(box); io_boxes[handle] = NULL;
    io_free[io_free_count++] = handle;
  } else {
    unsigned bin = io_box_class(box->capacity);
    int32_t *pool = input ? io_input_pool : io_pool;
    box->length = (uint32_t)pool[bin];
    pool[bin] = handle + 1;
    io_box_pool_count++; io_box_pool_bytes += box->capacity;
  }
}
static inline void io_box_drop(int32_t handle) { io_box_drop_from(handle, false); }
static inline void io_input_drop(int32_t handle) { io_box_drop_from(handle, true); }

static inline bool io_box_room_from(int32_t handle, uint64_t extra, bool input) {
  assert(handle >= 0 && handle < io_box_count);
  io_box *box = io_boxes[handle];
  assert(box != NULL);
  uint64_t needed = box->length + extra;
  if (needed > IO_BOX_MAX) return false;
  if (needed <= box->capacity) return true;
  uint32_t capacity = box->capacity;
  while (capacity < needed) capacity *= 2;
  if (input && io_input_native_memory + capacity - box->capacity > IO_INPUT_NATIVE_LIMIT) return false;
  if (io_box_memory + io_input_memory + capacity - box->capacity > IO_BOX_LIMIT) return false;
  uint32_t old = box->capacity;
  io_box *grown = realloc(box, sizeof *box + capacity);
  if (!grown) return false;
  grown->capacity = capacity; io_boxes[handle] = grown;
  io_box_memory += capacity - old;
  if (input) io_input_native_memory += capacity - old;
  io_box_grew();
  return true;
}
static inline bool io_box_room(int32_t handle, uint64_t extra) { return io_box_room_from(handle, extra, false); }

static inline uint8_t io_box_append_from(int32_t handle, sl_u8 bytes, bool input) {
  if (!io_box_room_from(handle, (uint64_t)bytes.len, input)) return false;
  io_box *box = io_boxes[handle];
  if (bytes.len) memcpy(box->bytes + box->length, bytes.data, (size_t)bytes.len);
  box->length += (uint32_t)bytes.len;
  return true;
}
static inline uint8_t io_box_append(int32_t handle, sl_u8 bytes) { return io_box_append_from(handle, bytes, false); }
static inline uint8_t io_input_append(int32_t handle, sl_u8 bytes) { return io_box_append_from(handle, bytes, true); }

static inline uint8_t io_box_append2(int32_t handle, sl_u8 first, sl_u8 second) {
  if (!io_box_room(handle, (uint64_t)first.len + (uint64_t)second.len)) return false;
  io_box *box = io_boxes[handle];
  if (first.len) memcpy(box->bytes + box->length, first.data, (size_t)first.len);
  if (second.len) memcpy(box->bytes + box->length + first.len, second.data, (size_t)second.len);
  box->length += (uint32_t)(first.len + second.len);
  return true;
}

static inline uint32_t io_box_len(int32_t handle) { return io_boxes[handle]->length; }
static inline void io_box_cut(int32_t handle, uint32_t length) {
  assert(length <= io_boxes[handle]->length);
  io_boxes[handle]->length = length;
}
static inline void io_box_read(int32_t handle, gs_rref out) {
  io_box *box = io_boxes[handle];
  gs_bld_append(out, box->bytes, box->length);
}

static inline int32_t io_ring_add(int32_t slots) {
  if (io_ring_count >= IO_RINGS) return -ENOSPC;
  io_ring *ring = calloc(1, sizeof *ring);
  if (!ring) return -ENOMEM;
  struct io_uring_params params = {0};
  params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE;
  params.cq_entries = 65536;
  int rc = io_uring_queue_init_params(4096, &ring->queue, &params);
  if (rc < 0) { free(ring); return rc; }
  rc = io_uring_register_files_sparse(&ring->queue, (unsigned)slots);
  if (rc < 0) goto fail;
  if (posix_memalign((void **)&ring->buffers, 4096, IO_RX * sizeof(struct io_uring_buf))) {
    rc = -ENOMEM; goto fail;
  }
  memset(ring->buffers, 0, IO_RX * sizeof(struct io_uring_buf));
  ring->bytes = malloc(IO_RX * IO_BYTES);
  if (!ring->bytes) { rc = -ENOMEM; goto fail; }
  struct io_uring_buf_reg reg = {0};
  reg.ring_addr = (uint64_t)(uintptr_t)ring->buffers;
  reg.ring_entries = IO_RX;
  rc = (int)syscall(__NR_io_uring_register, ring->queue.ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  if (rc < 0 && errno == EINVAL) {
    /* Ubuntu's inverted reserved-word check, as handled by the scored Rust server. */
    reg.resv[0] = 1;
    rc = (int)syscall(__NR_io_uring_register, ring->queue.ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
  }
  if (rc < 0) { rc = -errno; goto fail; }
  io_uring_buf_ring_init(ring->buffers);
  for (unsigned i = 0; i < IO_RX; i++)
    io_uring_buf_ring_add(ring->buffers, ring->bytes + i * IO_BYTES, IO_BYTES, (unsigned short)i, IO_RX - 1, (int)i);
  io_uring_buf_ring_advance(ring->buffers, IO_RX);
  io_rings[io_ring_count] = ring;
  return io_ring_count++;
fail:
  io_uring_queue_exit(&ring->queue);
  free(ring->bytes); free(ring->buffers); free(ring);
  return rc;
}

static inline struct io_uring_sqe *io_sqe(io_ring *ring) {
  struct io_uring_sqe *entry = io_uring_get_sqe(&ring->queue);
  if (entry) return entry;
  /* One bounded retry. A persistent submission failure is reported to Goose. */
  int rc = io_uring_submit_and_get_events(&ring->queue);
  if (rc < 0) return NULL;
  return io_uring_get_sqe(&ring->queue);
}

static inline int32_t io_accept(int32_t ri, int32_t listener, uint64_t ud) {
  struct io_uring_sqe *e = io_sqe(io_rings[ri]);
  if (!e) return -EIO;
  io_uring_prep_multishot_accept_direct(e, io_listeners[listener], NULL, NULL, 0);
  io_uring_sqe_set_data64(e, ud);
  return 0;
}
static inline int32_t io_cancel_accept(int32_t ri, uint64_t accept_ud, uint64_t ud) {
  struct io_uring_sqe *e = io_sqe(io_rings[ri]);
  if (!e) return -EIO;
  io_uring_prep_cancel64(e, accept_ud, 0);
  io_uring_sqe_set_data64(e, ud);
  return 0;
}
static inline int32_t io_receive_buffer_size(void) { return IO_BYTES; }
static inline int32_t io_recv(int32_t ri, int32_t slot, uint64_t ud) {
  struct io_uring_sqe *e = io_sqe(io_rings[ri]);
  if (!e) return -EIO;
  io_uring_prep_recv_multishot(e, slot, NULL, 0, 0);
  e->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  e->buf_group = 0; io_uring_sqe_set_data64(e, ud);
  return 0;
}
static inline int32_t io_read(int32_t ri, int32_t slot, uint64_t ud) {
  struct io_uring_sqe *e = io_sqe(io_rings[ri]);
  if (!e) return -EIO;
  io_uring_prep_recv(e, slot, NULL, 0, MSG_DONTWAIT);
  e->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
  e->buf_group = 0; io_uring_sqe_set_data64(e, ud);
  return 0;
}
static inline int32_t io_send(int32_t ri, int32_t slot, int32_t handle, uint32_t offset, uint64_t ud) {
  io_box *box = io_boxes[handle];
  assert(box != NULL && offset < box->length);
  struct io_uring_sqe *e = io_sqe(io_rings[ri]);
  if (!e) return -EIO;
  io_uring_prep_send(e, slot, box->bytes + offset, box->length - offset, MSG_NOSIGNAL);
  e->flags |= IOSQE_FIXED_FILE; io_uring_sqe_set_data64(e, ud);
  return 0;
}
static inline int32_t io_close(int32_t ri, int32_t slot, uint64_t shutdown_ud, uint64_t close_ud) {
  io_ring *ring = io_rings[ri];
  if (io_uring_sq_space_left(&ring->queue) < 2) {
    int rc = io_uring_submit_and_get_events(&ring->queue);
    if (rc < 0) return rc;
  }
  if (io_uring_sq_space_left(&ring->queue) < 2) return -EIO;
  struct io_uring_sqe *a = io_uring_get_sqe(&ring->queue);
  struct io_uring_sqe *b = io_uring_get_sqe(&ring->queue);
  assert(a && b);
  io_uring_prep_shutdown(a, slot, SHUT_RDWR);
  a->flags |= IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
  io_uring_sqe_set_data64(a, shutdown_ud);
  io_uring_prep_close_direct(b, (unsigned)slot);
  io_uring_sqe_set_data64(b, close_ud);
  return 0;
}

static inline int32_t io_enter(int32_t ri, uint8_t wait) {
  io_ring *ring = io_rings[ri];
  int rc;
  if (wait) {
    struct __kernel_timespec timeout = {.tv_sec = 0, .tv_nsec = 1000000};
    struct io_uring_cqe *completion;
    rc = io_uring_submit_and_wait_timeout(&ring->queue, &completion, 256, &timeout, NULL);
  } else rc = io_uring_submit_and_get_events(&ring->queue);
  return rc == -ETIME || rc == -EINTR || rc == -EBUSY || rc == -EAGAIN ? 0 : rc;
}
static inline int32_t io_submit_pending(void) {
  for (int i = 0; i < io_ring_count; i++) {
    if (io_uring_sq_ready(&io_rings[i]->queue)) {
      int rc = io_enter(i, 0);
      if (rc < 0) return rc;
    }
  }
  return 0;
}
static inline void io_reap(int32_t ri, gs_rref out) {
  io_ring *ring = io_rings[ri];
  struct io_uring_cqe *entry;
  unsigned head, count = 0;
  io_uring_for_each_cqe(&ring->queue, head, entry) {
    /* Goose's packed Cqe is exactly the kernel record's 8+4+4 bytes. */
    uint8_t *p = (uint8_t *)out.stk->top;
    memcpy(p, &entry->user_data, 8); memcpy(p + 8, &entry->res, 4); memcpy(p + 12, &entry->flags, 4);
    out.stk->top += 16; out.hdr->len++; count++;
  }
  io_uring_cq_advance(&ring->queue, count);
}
static inline void io_return(int32_t ri, int32_t buffer) {
  assert(buffer >= 0 && buffer < IO_RX);
  io_ring *ring = io_rings[ri];
  io_uring_buf_ring_add(ring->buffers, ring->bytes + buffer * IO_BYTES, IO_BYTES,
      (unsigned short)buffer, IO_RX - 1, (int)ring->returned++);
}
static inline void io_take(int32_t ri, int32_t buffer, int32_t length, gs_rref out) {
  assert(length >= 0 && length <= IO_BYTES);
  assert(buffer >= 0 && buffer < IO_RX);
  gs_bld_append(out, io_rings[ri]->bytes + buffer * IO_BYTES, length);
  io_return(ri, buffer);
}
static inline void io_publish(void) {
  for (int i = 0; i < io_ring_count; i++) {
    io_ring *ring = io_rings[i];
    io_uring_buf_ring_advance(ring->buffers, (int)ring->returned);
    ring->returned = 0;
  }
}

/* A stop request is checked only between completed batches. */
volatile sig_atomic_t io_stopping;
static inline uint8_t io_stop(void) { return io_stopping != 0; }
static inline int32_t io_listen(sl_u8 host, int32_t port) {
  char address[64]; struct in_addr ip;
  if (host.len <= 0 || host.len >= (int64_t)sizeof address) return -EINVAL;
  memcpy(address, host.data, (size_t)host.len); address[host.len] = 0;
  if (inet_pton(AF_INET, address, &ip) != 1) return -EINVAL;
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < 8; i++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one)) {
      int e = errno; close(fd); return -e;
    }
    struct sockaddr_in endpoint = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = ip};
    if (bind(fd, (struct sockaddr *)&endpoint, sizeof endpoint) || listen(fd, 4096)) {
      int e = errno; close(fd); return -e;
    }
    io_listeners[i] = fd;
  }
  return 0;
}
static inline int32_t io_slots(void) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit)) return -errno;
  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit)) return -errno;
  return (int32_t)(limit.rlim_max < 65535 ? limit.rlim_max : 65535);
}
static inline void io_lock_code(void) {
  /* Best effort, process-local: keep executable pages resident near memory pressure. */
  char path[4096], line[8192];
  ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
  if (length < 0) return;
  path[length] = 0;
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) return;
  while (fgets(line, sizeof line, maps)) {
    unsigned long start, end; char permissions[5];
    if (strstr(line, path) && sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 && permissions[0] == 'r')
      (void)mlock((void *)start, end - start); /* Permission/limit failure is harmless. */
  }
  fclose(maps);
}
static inline uint8_t io_env(sl_u8 key, gs_rref out) {
  char name[128];
  if (key.len <= 0 || key.len >= (int64_t)sizeof name) return false;
  memcpy(name, key.data, (size_t)key.len); name[key.len] = 0;
  const char *value = getenv(name);
  if (value) gs_bld_append(out, value, (int64_t)strlen(value));
  return value != NULL;
}
static inline double io_now(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_REALTIME, &t)) { perror("clock_gettime"); exit(1); }
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static inline int64_t io_micros(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static inline void io_log(sl_u8 message) {
  fwrite(message.data, 1, (size_t)message.len, stderr); fputc('\n', stderr);
}
#endif
