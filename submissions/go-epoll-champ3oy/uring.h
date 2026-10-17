// A thin io_uring layer over liburing for uring.go. Client sockets are direct descriptors:
// they live in each ring's registered file table and never get a file descriptor, so
// RLIMIT_NOFILE only bounds the size of each table, not the number of connections.
#ifndef URING_H
#define URING_H

#include <stdint.h>

typedef struct {
  uint64_t ud;
  int32_t res;
  uint32_t flags;
} ur_cqe;

// Sets up nrings rings, each with `slots` direct-descriptor slots and a pool of `nbufs`
// provided receive buffers of `bufsize` bytes. Returns 0 or -errno.
int ur_setup(int nrings, int slots, int nbufs, int bufsize);
char *ur_bufs(int ring);

// Queue one request; they're submitted by ur_wait. Return 0 or -EBUSY when the SQ is full.
int ur_accept(int ring, int lfd, uint64_t ud);   // multishot accept into a free slot
int ur_cancel_accept(int ring, int lfd);
int ur_recv(int ring, int slot, uint64_t ud);    // multishot recv into a pool buffer
int ur_send(int ring, int slot, const void *p, int len, uint64_t ud);
int ur_close(int ring, int slot, uint64_t ud);   // cancels the slot's requests, then closes it
int ur_poll(int ring, int fd, uint64_t ud);      // multishot POLLIN on a normal descriptor

void ur_buf_return(int ring, int bid);

// Submits every ring's queued requests, waits until there's at least one completion and
// copies up to max of them into out. Returns the count, or -errno.
int ur_wait(ur_cqe *out, int max);

#endif
