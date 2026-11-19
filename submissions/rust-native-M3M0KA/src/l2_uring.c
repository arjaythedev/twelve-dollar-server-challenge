#define _GNU_SOURCE
#include <liburing.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define L2_BUFFER_SIZE 2048u
#define L2_MAX_BUFFER_COUNT 32768u

struct l2_ring {
    struct io_uring ring;
    int event_fd;
    uint32_t file_slots;
    uint16_t buffer_group;
    uint32_t buffer_count;
    int buf_ring_registered;
    struct io_uring_buf_ring *buf_ring;
    unsigned char *buffers;
};

struct l2_cqe {
    uint64_t user_data;
    int32_t result;
    uint32_t flags;
};

struct l2_ring *l2_ring_create(uint32_t entries, uint32_t cq_entries,
                               uint32_t file_slots, uint16_t buffer_group,
                               uint32_t buffer_count, int *error_out) {
    if (error_out) *error_out = -EINVAL;
    if (!entries || !cq_entries || !file_slots || !buffer_count ||
        buffer_count > L2_MAX_BUFFER_COUNT ||
        (buffer_count & (buffer_count - 1u)) != 0) {
        return NULL;
    }

    struct l2_ring *state = calloc(1, sizeof(*state));
    if (!state) {
        if (error_out) *error_out = -ENOMEM;
        return NULL;
    }
    state->event_fd = -1;
    state->file_slots = file_slots;
    state->buffer_group = buffer_group;
    state->buffer_count = buffer_count;

    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
                   IORING_SETUP_CQSIZE;
    params.cq_entries = cq_entries;
    int rc = io_uring_queue_init_params(entries, &state->ring, &params);
    if (rc < 0) {
        if (error_out) *error_out = rc;
        goto fail;
    }

    rc = io_uring_register_files_sparse(&state->ring, file_slots);
    if (rc < 0) {
        if (error_out) *error_out = rc;
        goto fail_ring;
    }

    size_t buf_bytes = (size_t)buffer_count * L2_BUFFER_SIZE;
    rc = posix_memalign((void **)&state->buffers, 4096, buf_bytes);
    if (rc != 0) {
        state->buffers = NULL;
        if (error_out) *error_out = -rc;
        goto fail_ring;
    }
    memset(state->buffers, 0, buf_bytes);

    size_t ring_bytes =
        (((size_t)buffer_count * sizeof(struct io_uring_buf)) + 4095u) &
        ~(size_t)4095u;
    rc = posix_memalign((void **)&state->buf_ring, 4096, ring_bytes);
    if (rc != 0) {
        state->buf_ring = NULL;
        if (error_out) *error_out = -rc;
        goto fail_buffers;
    }
    memset(state->buf_ring, 0, ring_bytes);

    struct io_uring_buf_reg reg = {0};
    reg.ring_addr = (unsigned long)(uintptr_t)state->buf_ring;
    reg.ring_entries = buffer_count;
    reg.bgid = buffer_group;
    rc = io_uring_register_buf_ring(&state->ring, &reg, 0);
    if (rc == -EINVAL) {
        /* Ubuntu 6.8.0-136+ (LP #2162843) rejects zeroed resv[0]; retry once. */
        memset(&reg, 0, sizeof(reg));
        reg.ring_addr = (unsigned long)(uintptr_t)state->buf_ring;
        reg.ring_entries = buffer_count;
        reg.bgid = buffer_group;
        reg.resv[0] = 1;
        rc = io_uring_register_buf_ring(&state->ring, &reg, 0);
    }
    if (rc < 0) {
        if (error_out) *error_out = rc;
        goto fail_buf_ring;
    }
    state->buf_ring_registered = 1;

    io_uring_buf_ring_init(state->buf_ring);
    int mask = io_uring_buf_ring_mask(buffer_count);
    for (uint32_t i = 0; i < buffer_count; i++) {
        io_uring_buf_ring_add(state->buf_ring,
                              state->buffers + (size_t)i * L2_BUFFER_SIZE,
                              L2_BUFFER_SIZE, (unsigned short)i, mask, (int)i);
    }
    io_uring_buf_ring_advance(state->buf_ring, (int)buffer_count);

    state->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state->event_fd < 0) {
        if (error_out) *error_out = -errno;
        goto fail_buf_ring_reg;
    }
    rc = io_uring_register_eventfd(&state->ring, state->event_fd);
    if (rc < 0) {
        if (error_out) *error_out = rc;
        goto fail_eventfd;
    }
    if (error_out) *error_out = 0;
    return state;

fail_eventfd:
    close(state->event_fd);
fail_buf_ring_reg:
    if (state->buf_ring_registered) {
        (void)io_uring_unregister_buf_ring(&state->ring, state->buffer_group);
    }
fail_buf_ring:
    free(state->buf_ring);
fail_buffers:
    free(state->buffers);
fail_ring:
    io_uring_queue_exit(&state->ring);
fail:
    free(state);
    return NULL;
}

int l2_ring_event_fd(const struct l2_ring *state) {
    return state ? state->event_fd : -1;
}

static struct io_uring_sqe *l2_get_sqe(struct l2_ring *state) {
    return state ? io_uring_get_sqe(&state->ring) : NULL;
}

int l2_submit(struct l2_ring *state) {
    if (!state) return -EINVAL;
    int rc = io_uring_submit(&state->ring);
    return rc < 0 ? rc : rc;
}

int l2_prep_accept(struct l2_ring *state, int listener, unsigned slot,
                   uint64_t user_data) {
    if (!state || listener < 0 || slot >= state->file_slots) return -EINVAL;
    struct io_uring_sqe *sqe = l2_get_sqe(state);
    if (!sqe) return -EAGAIN;
    /* SOCK_CLOEXEC is rejected for direct accepts on Linux 6.8. */
    io_uring_prep_accept_direct(sqe, listener, NULL, NULL, SOCK_NONBLOCK, slot);
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

int l2_prep_recv_select(struct l2_ring *state, unsigned slot,
                        uint64_t user_data) {
    if (!state || slot >= state->file_slots) return -EINVAL;
    struct io_uring_sqe *sqe = l2_get_sqe(state);
    if (!sqe) return -EAGAIN;
    io_uring_prep_recv(sqe, (int)slot, NULL, 0, 0);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->buf_group = state->buffer_group;
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

int l2_return_buffer(struct l2_ring *state, unsigned buffer_id) {
    if (!state || !state->buf_ring || buffer_id >= state->buffer_count)
        return -EINVAL;
    int mask = io_uring_buf_ring_mask(state->buffer_count);
    io_uring_buf_ring_add(state->buf_ring,
                          state->buffers + (size_t)buffer_id * L2_BUFFER_SIZE,
                          L2_BUFFER_SIZE, (unsigned short)buffer_id, mask, 0);
    io_uring_buf_ring_advance(state->buf_ring, 1);
    return 0;
}

void *l2_buffer_ptr(struct l2_ring *state, unsigned buffer_id) {
    if (!state || buffer_id >= state->buffer_count) return NULL;
    return state->buffers + (size_t)buffer_id * L2_BUFFER_SIZE;
}

uint32_t l2_buffer_size(void) { return L2_BUFFER_SIZE; }

uint32_t l2_ring_buffer_count(const struct l2_ring *state) {
    return state ? state->buffer_count : 0;
}

uint32_t l2_cq_ready(struct l2_ring *state) {
    return state ? io_uring_cq_ready(&state->ring) : 0;
}

int l2_prep_send(struct l2_ring *state, unsigned slot, const void *buffer,
                 unsigned length, uint64_t user_data) {
    if (!state || slot >= state->file_slots || !buffer || !length) return -EINVAL;
    struct io_uring_sqe *sqe = l2_get_sqe(state);
    if (!sqe) return -EAGAIN;
    io_uring_prep_send(sqe, (int)slot, buffer, length, MSG_NOSIGNAL);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

int l2_prep_close(struct l2_ring *state, unsigned slot, uint64_t user_data) {
    if (!state || slot >= state->file_slots) return -EINVAL;
    struct io_uring_sqe *sqe = l2_get_sqe(state);
    if (!sqe) return -EAGAIN;
    io_uring_prep_close_direct(sqe, slot);
    io_uring_sqe_set_data64(sqe, user_data);
    return 0;
}

static int l2_copy_cqe(struct io_uring_cqe *cqe, struct l2_cqe *out) {
    if (!cqe || !out) return -EINVAL;
    out->user_data = cqe->user_data;
    out->result = cqe->res;
    out->flags = cqe->flags;
    return 0;
}

int l2_peek_cqe(struct l2_ring *state, struct l2_cqe *out) {
    if (!state || !out) return -EINVAL;
    struct io_uring_cqe *cqe = NULL;
    int rc = io_uring_peek_cqe(&state->ring, &cqe);
    if (rc < 0) return rc;
    if (l2_copy_cqe(cqe, out) < 0) return -EINVAL;
    io_uring_cqe_seen(&state->ring, cqe);
    return 1;
}

int l2_wait_cqe(struct l2_ring *state, struct l2_cqe *out) {
    if (!state || !out) return -EINVAL;
    struct io_uring_cqe *cqe = NULL;
    int rc = io_uring_wait_cqe(&state->ring, &cqe);
    if (rc < 0) return rc;
    if (l2_copy_cqe(cqe, out) < 0) return -EINVAL;
    io_uring_cqe_seen(&state->ring, cqe);
    return 0;
}

int l2_wait_many(struct l2_ring *const *rings, uint32_t count,
                 int timeout_ms, uint64_t *ready_mask) {
    if (!rings || !ready_mask || !count || count > 64 || timeout_ms < -1)
        return -EINVAL;
    struct pollfd fds[64];
    for (uint32_t i = 0; i < count; i++) {
        if (!rings[i] || rings[i]->event_fd < 0) return -EINVAL;
        fds[i] = (struct pollfd){.fd = rings[i]->event_fd, .events = POLLIN};
    }
    int rc = poll(fds, count, timeout_ms);
    if (rc < 0) return -errno;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        if (!(fds[i].revents & POLLIN)) continue;
        mask |= UINT64_C(1) << i;
        uint64_t count_value;
        for (;;) {
            ssize_t n = read(fds[i].fd, &count_value, sizeof(count_value));
            if (n == (ssize_t)sizeof(count_value)) continue;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0) return -errno;
            if (n == 0) break;
            return -EIO;
        }
    }
    *ready_mask = mask;
    return rc;
}

void l2_ring_destroy(struct l2_ring *state) {
    if (!state) return;
    io_uring_unregister_eventfd(&state->ring);
    if (state->buf_ring_registered) {
        (void)io_uring_unregister_buf_ring(&state->ring, state->buffer_group);
    }
    io_uring_queue_exit(&state->ring);
    if (state->event_fd >= 0) close(state->event_fd);
    free(state->buf_ring);
    free(state->buffers);
    free(state);
}
