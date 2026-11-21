/* From the submission directory: cc -O1 tests/held-writes.c -o /tmp/held && /tmp/held
 * Exercise the actual reactor with a deterministic database gate and an in-memory SQ.
 * Works with either kernel-selected buffers or application-owned receive credits. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../reactor.c"
#include <assert.h>

enum { BURST = PASS_REPLIES + 17, SLOTS = 2 * BURST + 3 };
static Connection clients[SLOTS];
static struct io_uring_sqe submissions[2048];
static unsigned sq_head, commits, writes_in_pass, records, order[SLOTS + 4];
static int gate_closed, reject_commit, error_responses;

void db_begin_pass(void) { writes_in_pass = 0; }
int db_writes_deferred(void) { return gate_closed; }
int db_pass_read_only(void) { return writes_in_pass == 0; }
int db_commit_pass(void) {
    ++commits;
    if (writes_in_pass) gate_closed = 1;
    return reject_commit ? -1 : 0;
}
void app_error_500(Buf *out) {
    ++error_responses;
    *out = (Buf){.data = "!", .len = 1, .cap = 1};
}
int app_request(const char *wire, size_t n, size_t *used, Buf *out, int *close_after) {
    const char *end = memchr(wire, '\n', n);
    if (!end) return 0;
    const char *space = memchr(wire, ' ', (size_t)(end - wire));
    assert(space && space < end && records < sizeof(order) / sizeof(order[0]));
    order[records++] = (unsigned)strtoul(space + 1, NULL, 10);
    writes_in_pass += wire[0] == 'P';
    *used = (size_t)(end - wire) + 1;
    *close_after = 0;
    *out = (Buf){.data = "x", .len = 1, .cap = 1};
    return 1;
}
static void input(unsigned slot, const char *wire) {
    Connection *c = &clients[slot];
    if (!c->generation) accept_conn(0, slot);
    dispatch(key(0, slot, c->generation), c, wire, strlen(wire), READY_INPUT);
}
static void commit_and_ack(void) {
    Ring *r = &rings[0];
    unsigned before = commits;
    finish_pass();
    assert(commits == before + 1);
    while (sq_head < r->tail) {
        struct io_uring_sqe *s = &submissions[sq_head++ & r->sq_mask];
        assert(s->opcode == IORING_OP_SEND);
        struct io_uring_cqe done = {.user_data = s->user_data, .res = (int)s->len};
        on_cqe(r, &done);
    }
}
static void check_fragmented_pipeline(void) {
    gate_closed = 1;
    budget = PASS_REPLIES;
    input(0, "P");
    input(0, "OS");
    Ring *r = &rings[0];
    assert(r->tail == sq_head + 1);
    const struct io_uring_sqe *hint = &submissions[sq_head++ & r->sq_mask];
    assert(hint->opcode == IORING_OP_URING_CMD && hint->fd == 0);
    assert(hint->cmd_op == SOCKET_URING_OP_SETSOCKOPT && (hint->flags & IOSQE_FIXED_FILE));
    assert(hint->level == IPPROTO_TCP && hint->optname == TCP_QUICKACK);
    assert(hint->optlen == sizeof(int) && *(const int *)(uintptr_t)hint->optval == 1);
    input(0, "T 10\nGE");
    input(0, "T 11\n");
    drain_ready();
    gate_closed = 0; /* Release after this pass last checked the write queue. */
    input(1, "POST 20\n");
    assert(records == 0);
    input(2, "GET 30\n");
    assert(records == 1 && order[0] == 30);
    drain_ready();
    assert(records == 3 && order[1] == 10 && order[2] == 20);
    assert(!(clients[0].flags & FRAGMENT_ACK));
    commit_and_ack();
    budget = PASS_REPLIES;
    drain_ready();
    assert(records == 4 && order[3] == 11);
    commit_and_ack();
}
static void check_backlog_fairness(void) {
    for (unsigned i = 0; i < BURST; ++i) {
        char line[32];
        budget = PASS_REPLIES;
        snprintf(line, sizeof(line), "POST %u\n", i);
        input(3 + i, line);
        budget = 0;
        snprintf(line, sizeof(line), "GET %u\n", BURST + i);
        input(3 + BURST + i, line);
    }
    assert(records == 4);
    for (unsigned seen = 0; seen < BURST;) {
        gate_closed = 0;
        budget = PASS_REPLIES;
        unsigned before = records, take = BURST - seen;
        if (take > PASS_REPLIES / 2) take = PASS_REPLIES / 2;
        drain_ready();
        assert(records == before + 2 * take);
        for (unsigned i = 0; i < take; ++i) {
            assert(order[before + i] == seen + i);
            assert(order[before + take + i] == BURST + seen + i);
        }
        commit_and_ack();
        seen += take;
    }
    assert(!has_items(&ready) && !has_items(&held_writes));
}
static void check_failed_commit(void) {
    Ring *r = &rings[0];
    unsigned before = r->tail;
    gate_closed = 0;
    reject_commit = 1;
    budget = PASS_REPLIES;
    input(0, "POST 111\n");
    input(1, "GET 112\n");
    assert(staged_count == 2 && r->tail == before && !error_responses);
    flush_bank();
    assert(!staged_count && error_responses == 2 && r->tail == before + 2);
    for (unsigned i = before; i < r->tail; ++i) {
        struct io_uring_sqe *s = &submissions[i & r->sq_mask];
        assert(s->opcode == IORING_OP_SEND && s->len == 1);
        assert(*(const char *)(uintptr_t)s->addr == '!');
    }
}
static void check_stale_receive_credits(void) {
    enum { N = PARK_WINDOW + 17 };
    static Connection parked[N];
    Ring *r = &rings[0];
    r->conns = parked;
    r->tail = sq_head = allocated = unused = 0;
    uint64_t first_seen = monotonic_ns() - UINT64_C(1000000000);
    pending.stamp = first_seen;
    for (unsigned i = 0; i < N; ++i) {
        parked[i].generation = 1;
        parked[i].flags = OPEN | PARKED;
        retry_receive(r, key(0, i, 1), &parked[i]);
    }
    pending.stamp = first_seen + UINT64_C(100000000);
    retry_receive(r, key(0, 0, 1), &parked[0]);
    assert(pending.count == N && pending.items[pending.head].since == first_seen);
    ++parked[1].generation; /* Stale while queued. */
    assert(issue_park() == PARK_WINDOW && issue_park() == 0);
    assert(submissions[0].fd == 0 && submissions[1].fd == 2);

    int selected = (submissions[0].flags & IOSQE_BUFFER_SELECT) != 0;
    for (unsigned i = 0; i < PARK_WINDOW; ++i) {
        struct io_uring_sqe *s = &submissions[i];
        assert(s->opcode == IORING_OP_RECV && (s->flags & IOSQE_FIXED_FILE));
        assert(((s->flags & IOSQE_BUFFER_SELECT) != 0) == selected);
        if (!selected) {
            assert(s->addr && s->len);
            for (unsigned j = 0; j < i; ++j) assert(submissions[j].addr != s->addr);
        }
        ++parked[s->fd].generation; /* Stale in flight, in either buffer implementation. */
        struct io_uring_cqe done = {.user_data = s->user_data, .res = -EAGAIN};
        on_cqe(r, &done);
    }
    assert(!pending.inflight);
    /* Fill a second complete window. Even one leaked application buffer would
     * force a selected-buffer fallback for the last read. */
    unsigned replacements = PARK_WINDOW - pending.count;
    for (unsigned i = 0; i < replacements; ++i) {
        unsigned slot = (unsigned)submissions[i].fd;
        Connection *c = &parked[slot];
        c->flags = OPEN | PARKED;
        retry_receive(r, key(0, slot, c->generation), c);
    }
    sq_head = r->tail;
    assert(issue_park() == PARK_WINDOW && !pending.count);
    for (unsigned i = sq_head; i < r->tail; ++i) {
        struct io_uring_sqe *s = &submissions[i];
        assert(((s->flags & IOSQE_BUFFER_SELECT) != 0) == selected);
        if (!selected) {
            unsigned matches = 0;
            for (unsigned j = 0; j < PARK_WINDOW; ++j) matches += s->addr == submissions[j].addr;
            assert(matches == 1);
            for (unsigned j = sq_head; j < i; ++j) assert(submissions[j].addr != s->addr);
        }
        struct io_uring_cqe done = {.user_data = s->user_data, .res = -EAGAIN};
        on_cqe(r, &done);
    }
    assert(!pending.inflight);
}
int main(void) {
    rings[0].conns = clients;
    rings[0].sq_head = &sq_head;
    rings[0].sqes = submissions;
    rings[0].sq_mask = sizeof(submissions) / sizeof(submissions[0]) - 1;
    check_fragmented_pipeline();
    check_backlog_fairness();
    check_failed_commit();
    check_stale_receive_credits();
    puts("held-write FIFO, fragmented pipeline, read fairness, failed commit and stale receive credits passed");
}
