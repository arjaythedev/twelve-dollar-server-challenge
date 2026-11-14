/* GENERATED opcode-specific constructors; zero, initialize, then commit privately. */
static void sqe_accept(ring *r, int fd, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_ACCEPT; s->ioprio = IORING_ACCEPT_MULTISHOT; s->file_index = IORING_FILE_INDEX_ALLOC;
  s->fd = fd;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}

static void sqe_recv(ring *r, int fd, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_RECV; s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT; s->ioprio = IORING_RECV_MULTISHOT; s->buf_group = 0;
  s->fd = fd;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}

static void sqe_send(ring *r, int fd, u64 addr, u32 len, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_SEND; s->flags = IOSQE_FIXED_FILE; s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
  s->fd = fd;
  s->addr = addr;
  s->len = len;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}

static void sqe_shutdown(ring *r, int fd, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_SHUTDOWN; s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS; s->len = SHUT_RDWR;
  s->fd = fd;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}

static void sqe_close(ring *r, u32 index, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_CLOSE;
  s->file_index = index;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}

static void sqe_cancel(ring *r, u64 target, u64 tag) {
  sqe_reserved slot = sqe_reserve(r);
  struct io_uring_sqe *s = slot.p;
  s->opcode = IORING_OP_ASYNC_CANCEL; s->flags = IOSQE_CQE_SKIP_SUCCESS;
  s->addr = target;
  s->user_data = tag;
  sqe_commit(r, (sqe_ready){s});
}
