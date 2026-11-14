/* From the submission directory:
 * cc -std=gnu11 -O2 -Wall -Wextra -Werror tests/batch-watch.c -o batch-watch-test
 * ./batch-watch-test
 * Exercises kernel io_uring and epoll using local descriptors. Exit 77 means
 * the host cannot create the ring or import a file into its sparse table. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../reactor.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>

void db_begin_pass(void) {}
int db_pass_read_only(void) { return 1; }
int db_writes_deferred(void) { return 0; }
int db_commit_pass(void) { return 0; }
void app_error_500(Buf *out) { (void)out; abort(); }
int app_request(const char *req, size_t len, size_t *used, Buf *out, int *close_after) {
    (void)req;
    (void)len;
    (void)used;
    (void)out;
    (void)close_after;
    return 0;
}

enum Scenario { SHORT_PREFIX, REJECT_WATCH, EXISTING_WATCH, REUSED_SLOT };

static Ring *open_test_ring(void) {
    slots = 64;
    if (new_ring(0) < 0 || !probe_import(&rings[0])) {
        fputs("SKIP: io_uring sparse-file import unavailable\n", stderr);
        _exit(77);
    }
    park_fd = epoll_create1(EPOLL_CLOEXEC);
    assert(park_fd >= 0);
    register_bank(0);
    return &rings[0];
}

static void stage_alias(Ring *ring, int fd) {
    append_admission(ring, fd);
    ++ordinary_pending;
    ++ring->accepting;
}

static void complete_pending(Ring *ring) {
    while (*ring->cq_head == get_acquire(ring->cq_tail)) enter_ring(ring, 1, -1);
    assert(reap(ring) > 0);
}

static struct io_uring_cqe take_cqe(Ring *ring) {
    unsigned head = *ring->cq_head;
    assert(head != get_acquire(ring->cq_tail));
    struct io_uring_cqe event = ring->cqes[head & ring->cq_mask];
    put_release(ring->cq_head, head + 1);
    return event;
}

static void check_short_import(enum Scenario scenario) {
    Ring *ring = open_test_ring();
    int aliases[8], gaps[4], replacements[4];
    for (unsigned i = 0; i < 8; ++i) {
        aliases[i] = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
        assert(aliases[i] >= 0);
        if (i % 2 == 0) {
            gaps[i / 2] = eventfd(0, EFD_CLOEXEC);
            assert(gaps[i / 2] >= 0);
        }
        stage_alias(ring, aliases[i]);
    }
    unsigned id = ring->admission_batch;
    AdmissionBatch *batch = &admissions[id];
    flush_admission(ring);
    batch->indices[4] = -1; /* FILES_UPDATE installs the valid prefix of four. */
    complete_pending(ring);
    assert(ring->live == 4 && ring->accepting == 4 && ordinary_pending == 8);
    assert(!delayed_imports && !closing_count && batch->pending_watches == 4 && batch->prefix == 4);

    unsigned start = ring->tail - 4;
    if (scenario == REJECT_WATCH) {
        /* An opcode the kernel does not implement returns a real EINVAL CQE.
         * The other error arrives after its connection generation changes. */
        ring->sqes[(start + 1) & ring->sq_mask].opcode = UINT8_MAX;
        ring->sqes[(start + 2) & ring->sq_mask].len = UINT32_MAX;
        uint64_t stale = batch->watches[2].data.u64;
        ++ring->conns[which_slot(stale)].generation;
    }
    if (scenario == EXISTING_WATCH) {
        struct epoll_event previous = {.events = EPOLLIN | EPOLLET, .data.u64 = UINT64_MAX};
        assert(epoll_ctl(ring->parked_fd, EPOLL_CTL_ADD, aliases[1], &previous) == 0);
        assert(epoll_wait(ring->parked_fd, &previous, 1, 0) == 1);
        assert(previous.data.u64 == UINT64_MAX);
    }

    struct epoll_event edges[8];
    enter_ring(ring, 4, -1);
    int count = epoll_wait(ring->parked_fd, edges, 8, 0);
    int expected = 4;
    if (scenario == REJECT_WATCH) expected = 2;
    if (scenario == EXISTING_WATCH) expected = 3;
    assert(count == expected);
    for (int i = 0; i < count; ++i) {
        Connection *connection = find_conn(edges[i].data.u64);
        assert(connection && (connection->flags & PARKED)); /* Readiness preceded the ADD CQE. */
    }
    for (unsigned i = 0; i < 8; ++i) assert(fcntl(aliases[i], F_GETFD) >= 0);

    unsigned duplicates = 0;
    for (unsigned i = 0; i < 4; ++i) {
        struct io_uring_cqe event = take_cqe(ring);
        if (event.res == -EEXIST) ++duplicates;
        on_cqe(ring, &event);
        if (i < 3) assert(!closing_count && !delayed_imports);
    }
    assert(duplicates == (unsigned)(scenario == EXISTING_WATCH));
    assert(ring_watches == (scenario != REJECT_WATCH));
    assert(closing_count == 4 && delayed_imports == id && batch->count == 4 && !batch->pending_watches);
    if (scenario != SHORT_PREFIX) {
        /* Both synchronous fallback and a refreshed EEXIST registration must
         * deliver the current generation's key. The stale registration cannot. */
        assert(epoll_wait(ring->parked_fd, edges, 8, 0) == 1);
        assert(edges[0].data.u64 == batch->watches[1].data.u64 && find_conn(edges[0].data.u64));
    }

    close_aliases();
    assert(ordinary_pending == 4);
    for (unsigned i = 0; i < 4; ++i) {
        assert(fcntl(aliases[i], F_GETFD) < 0 && errno == EBADF);
        replacements[i] = eventfd(0, EFD_CLOEXEC);
        assert(replacements[i] == aliases[i]);
        assert(fcntl(aliases[i + 4], F_GETFD) >= 0 && fcntl(gaps[i], F_GETFD) >= 0);
    }
    batch->retry_at = 0;
    retry_admissions();
    complete_pending(ring);
    while (batch->pending_watches) complete_pending(ring);
    close_aliases();
    assert(ring->live == 8 && !ring->accepting && !ordinary_pending && !delayed_imports);
    assert(epoll_wait(ring->parked_fd, edges, 8, 0) == 4);
    for (unsigned i = 0; i < 4; ++i) {
        assert(fcntl(replacements[i], F_GETFD) >= 0 && fcntl(gaps[i], F_GETFD) >= 0);
        assert(fcntl(aliases[i + 4], F_GETFD) < 0 && errno == EBADF);
        assert(find_conn(edges[i].data.u64));
    }
}

static void check_reused_slot(void) {
    Ring *ring = open_test_ring();
    int sockets[4][2];
    for (unsigned i = 0; i < 4; ++i) {
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets[i]) == 0);
        stage_alias(ring, sockets[i][0]);
    }
    unsigned id = ring->admission_batch;
    AdmissionBatch *batch = &admissions[id];
    flush_admission(ring);
    complete_pending(ring);
    assert(batch->pending_watches == 4);
    ring->sqes[(ring->tail - 3) & ring->sq_mask].len = UINT32_MAX;
    enter_ring(ring, 4, -1);
    struct io_uring_cqe delayed = {0};
    for (unsigned i = 0; i < 4; ++i) {
        struct io_uring_cqe event = take_cqe(ring);
        if (event.res == -EINVAL) delayed = event;
        else {
            assert(event.res == 0);
            on_cqe(ring, &event);
        }
    }
    assert(delayed.res == -EINVAL && batch->pending_watches == 1 && !closing_count);
    uint64_t old_key = batch->watches[1].data.u64;
    unsigned slot = which_slot(old_key);
    Connection *old = find_conn(old_key);
    assert(old);
    close_conn(old_key, old);
    complete_pending(ring);
    assert(!find_conn(old_key) && ring->live == 3);
    char byte;
    assert(read(sockets[1][1], &byte, 1) == 0); /* Actual shutdown, while the batch still owns the fd. */
    assert(fcntl(sockets[1][0], F_GETFD) >= 0);

    int fresh[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fresh) == 0);
    struct io_uring_files_update update = {.offset = slot, .fds = (uintptr_t)&fresh[0]};
    assert(syscall(SYS_io_uring_register, ring->fd, IORING_REGISTER_FILES_UPDATE, &update, 1) == 1);
    uint64_t new_key = accept_conn(0, slot);
    Connection *replacement = find_conn(new_key);
    assert(new_key != old_key && replacement && replacement->flags == OPEN);
    unsigned tail = ring->tail;
    on_cqe(ring, &delayed);
    assert(find_conn(new_key) == replacement && replacement->flags == OPEN);
    assert(ring->tail == tail && closing_count == 4); /* No fallback read on the reused slot. */
    assert(write(fresh[1], "N", 1) == 1 && read(fresh[0], &byte, 1) == 1 && byte == 'N');
    close_aliases();
    assert(!ordinary_pending && fcntl(sockets[1][0], F_GETFD) < 0 && errno == EBADF);
    assert(fcntl(fresh[0], F_GETFD) >= 0);
    close_conn(new_key, replacement);
    complete_pending(ring);
    assert(read(fresh[1], &byte, 1) == 0);
}

int main(void) {
    for (enum Scenario scenario = SHORT_PREFIX; scenario <= REUSED_SLOT; ++scenario) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            alarm(10);
            if (scenario == REUSED_SLOT) check_reused_slot();
            else check_short_import(scenario);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) == 77) return 77;
        assert(WEXITSTATUS(status) == 0);
    }
    puts("short import, early readiness, unsupported watch, EEXIST refresh, suffix retry and stale-close reuse passed");
    return 0;
}
