/* From the submission directory: cc -O1 tests/wake.c -o /tmp/wake && /tmp/wake
 * Exercises real Linux epoll readiness, registered Unix sockets and io_uring.
 * Exit 77 means the kernel or sandbox does not expose the required ring features. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../reactor.c"
#include <assert.h>
#include <fcntl.h>

typedef struct {
    unsigned bank;
    uint64_t key;
    int peer;
} TestSocket;

/* Leave each fragment incomplete so dispatch retains the exact bytes that the
 * fixed-file receive got. No database, HTTP server or external process is needed. */
int app_request(const char *input, size_t n, size_t *used, Buf *out, int *close_after) {
    (void)input; (void)n; (void)used; (void)out; (void)close_after;
    return 0;
}
void app_error_500(Buf *out) { (void)out; abort(); }
void db_begin_pass(void) {}
int db_writes_deferred(void) { return 0; }
int db_pass_read_only(void) { return 1; }
int db_commit_pass(void) { return 0; }

static TestSocket add_socket(unsigned bank, unsigned slot) {
    Ring *r = &rings[bank];
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) == 0);
    struct io_uring_files_update update = {.offset = slot, .fds = (uintptr_t)&pair[0]};
    assert(syscall(SYS_io_uring_register, r->fd, IORING_REGISTER_FILES_UPDATE, &update, 1) == 1);
    uint64_t k = accept_conn(bank, slot);
    struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP | EPOLLET, .data.u64 = k};
    assert(epoll_ctl(r->parked_fd, EPOLL_CTL_ADD, pair[0], &event) == 0);
    find_conn(k)->flags |= PARKED;
    assert(close(pair[0]) == 0); /* Only the registered slot now owns this endpoint. */
    return (TestSocket){.bank = bank, .key = k, .peer = pair[1]};
}
static void send_byte(const TestSocket *s, char byte) {
    assert(send(s->peer, &byte, 1, MSG_NOSIGNAL) == 1);
}
static void consume_byte(const TestSocket *s, char expected) {
    Ring *r = &rings[s->bank];
    Connection *c = find_conn(s->key);
    assert(c && !(c->flags & READING));
    unsigned before = c->length;
    budget = PASS_REPLIES;
    assert(collect_bank(r, 1) == 1 && (c->flags & READING));
    uint64_t expires = monotonic_ns() + UINT64_C(500000000);
    do {
        enter_ring(r, 1, 20000000);
        reap(r);
        assert(monotonic_ns() < expires);
    } while (c->length == before);
    assert(c->length == before + 1 && c->input[before] == expected);
    assert(!(c->flags & READING) && !pending.inflight);
    finish_pass();
}
static unsigned root_completions(unsigned kind) {
    Ring *r = &rings[0];
    unsigned head = *r->cq_head, end = get_acquire(r->cq_tail), count = 0;
    for (unsigned i = head; i != end; ++i) {
        struct io_uring_cqe *c = &r->cqes[i & r->cq_mask];
        if (c->user_data >> 56 == kind) {
            assert(c->res >= 0 && (c->res & POLLIN));
            if (kind == PARKWAKE) assert(!(c->flags & IORING_CQE_F_MORE));
            ++count;
        }
    }
    if (kind == PARKWAKE) assert(count <= 1);
    if (head != end) reap(r);
    return count;
}
static void expect_wake(unsigned kind) {
    uint64_t expires = monotonic_ns() + UINT64_C(500000000);
    while (!root_completions(kind)) {
        assert(monotonic_ns() < expires);
        enter_ring(&rings[0], 1, 20000000);
    }
    if (kind == PARKWAKE) assert(!park_armed);
}
static void arm_idle(void) {
    arm_banks();
    arm_park_before_wait();
    assert(park_armed);
    unsigned tail = rings[0].tail;
    arm_park_before_wait();
    assert(rings[0].tail == tail);
}
static void drain_root(void) {
    enter_ring(&rings[0], 0, -1);
    reap(&rings[0]);
}
static void check_idle_arrivals(const TestSocket *first, const TestSocket *second) {
    /* Data can arrive after an empty child scan and before parent registration. */
    assert(!collect_bank(&rings[second->bank], 1));
    send_byte(second, 'a');
    arm_idle();
    expect_wake(PARKWAKE);
    consume_byte(second, 'a');
    assert(find_conn(first->key)->length == 0);

    /* The next arrival lands between creating and submitting the primary poll SQE. */
    arm_idle();
    send_byte(first, 'b');
    expect_wake(PARKWAKE);
    consume_byte(first, 'b');

    /* The poll has entered the kernel, and the child was empty at registration. */
    arm_banks();
    arm_park_before_wait();
    enter_ring(&rings[0], 0, -1);
    assert(rings[0].tail == get_acquire(rings[0].sq_head) && park_armed);
    send_byte(second, 'c');
    expect_wake(PARKWAKE);
    consume_byte(second, 'c');
}
static void check_secondary_notifications(const TestSocket *s) {
    Ring *r = &rings[s->bank];
    mute_secondaries();
    assert(*r->cq_flags & IORING_CQ_EVENTFD_DISABLED);
    drain_root();
    for (unsigned i = 0; i < 64; ++i) {
        send_byte(s, 'd');
        consume_byte(s, 'd');
        enter_ring(&rings[0], 0, -1);
        assert(*rings[0].cq_head == get_acquire(rings[0].cq_tail));
    }
    /* One child completion already exists while its eventfd is muted. */
    sqe(r, IORING_OP_NOP, op(IGNORE, 0), -1);
    enter_ring(r, 1, 100000000);
    assert(enable_and_recheck());
    assert(!(*r->cq_flags & IORING_CQ_EVENTFD_DISABLED));
    assert(reap(r) == 1);
    drain_root();
    /* A later completion must notify the primary after the acquire recheck. */
    sqe(r, IORING_OP_NOP, op(IGNORE, 0), -1);
    enter_ring(r, 1, 100000000);
    expect_wake(WAKE);
    assert(reap(r) == 1);
}

static void complete_import(Ring *r) {
    uint64_t expires = monotonic_ns() + UINT64_C(500000000);
    while (*r->cq_head == get_acquire(r->cq_tail)) {
        assert(monotonic_ns() < expires);
        enter_ring(r, 1, 20000000);
    }
    assert(reap(r) > 0);
}

static void check_partial_import(void) {
    assert(new_ring(2) == 0);
    Ring *r = &rings[2];
    if (!probe_import(r)) return;  /* The server falls back to direct accepts. */
    register_bank(2);
    int fds[4], gaps[2];
    for (unsigned i = 0; i < 4; ++i) {
        fds[i] = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
        assert(fds[i] >= 0);
        if (!(i & 1)) {
            gaps[i / 2] = eventfd(0, EFD_CLOEXEC);
            assert(gaps[i / 2] >= 0);
        }
        append_admission(r, fds[i]);
        ++r->accepting;
        ++ordinary_pending;
    }
    unsigned id = r->admission_batch;
    flush_admission(r);
    /* The kernel will publish the first two slots and leave the original
     * four descriptor numbers under the application's ownership. */
    admissions[id].indices[2] = -1;
    complete_import(r);
    assert(r->live == 2 && r->accepting == 2 && delayed_imports == id && closing_count == 2);
    close_aliases();
    assert(ordinary_pending == 2 && fcntl(fds[0], F_GETFD) == -1 && fcntl(fds[1], F_GETFD) == -1);
    int first_reuse = eventfd(0, EFD_CLOEXEC), second_reuse = eventfd(0, EFD_CLOEXEC);
    assert(first_reuse == fds[0] && second_reuse == fds[1]);
    admissions[id].retry_at = 0;
    retry_admissions();
    complete_import(r);
    close_aliases();
    assert(r->live == 4 && !r->accepting && !ordinary_pending && !delayed_imports);
    assert(fcntl(fds[2], F_GETFD) == -1 && fcntl(fds[3], F_GETFD) == -1);
    assert(fcntl(first_reuse, F_GETFD) >= 0 && fcntl(second_reuse, F_GETFD) >= 0);
    assert(fcntl(gaps[0], F_GETFD) >= 0 && fcntl(gaps[1], F_GETFD) >= 0);
    struct epoll_event events[4];
    assert(epoll_wait(r->parked_fd, events, 4, 0) == 4);
    for (unsigned i = 0; i < 4; ++i) {
        Connection *c = find_conn(events[i].data.u64);
        assert(c && (c->flags & PARKED));
    }
    close(first_reuse);
    close(second_reuse);
    close(gaps[0]);
    close(gaps[1]);
    puts("partial import, descriptor reuse and all four readiness watches passed");
}

int main(void) {
    slots = 32;
    if (new_ring(0) < 0 || new_ring(1) < 0) {
        perror("test io_uring setup");
        return 77;
    }
    if (!rings[1].cq_flags) {
        fputs("kernel does not expose CQ notification flags\n", stderr);
        return 77;
    }
    park_fd = epoll_create1(EPOLL_CLOEXEC);
    assert(park_fd >= 0);
    register_bank(0);
    register_bank(1);
    watch_ring(1);
    enter_ring(&rings[0], 0, -1);
    /* Identical fixed slot indices in two rings pin the routing check to the bank. */
    TestSocket first = add_socket(0, 3), second = add_socket(1, 3);
    /* Start the suppression check before posting any unmuted child work. A
     * previously signalled eventfd edge may legally appear after a later mute. */
    check_secondary_notifications(&second);
    check_idle_arrivals(&first, &second);
    check_partial_import();
    close(first.peer);
    close(second.peer);
    puts("fixed-slot Unix reads, idle rearm, busy suppression and secondary wakes passed");
}
