/* In-memory fixture for the explicit-slot reactor in reactor.patch. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define epoll_wait model_epoll_wait
#define epoll_ctl model_epoll_ctl
#include CANDIDATE_REACTOR
#undef epoll_ctl
#undef epoll_wait
#include <assert.h>

void db_begin_pass(void) {}
int db_pass_read_only(void) { return 1; }
int db_writes_deferred(void) { return 0; }
int db_commit_pass(void) { return 0; }
void app_error_500(Buf *out) { (void)out; abort(); }
int app_request(const char *req, size_t len, size_t *used, Buf *out, int *close_after) {
    (void)req; (void)len; (void)used; (void)out; (void)close_after;
    abort();
}

enum { FIRST_FD = 64, MODEL_SLOTS = 8, SQ_ENTRIES = 128 };
static struct epoll_event waiting[MODEL_SLOTS], watched[MODEL_SLOTS];
static unsigned waiting_count, refreshes;
static Connection connections[MODEL_SLOTS];
static struct io_uring_sqe submissions[SQ_ENTRIES];
static unsigned head;

int model_epoll_ctl(int port, int how, int fd, struct epoll_event *event) {
    assert(port == 21 && fd >= FIRST_FD && fd < FIRST_FD + MODEL_SLOTS);
    struct epoll_event *current = &watched[fd - FIRST_FD];
    if (how == EPOLL_CTL_ADD && current->events) {
        errno = EEXIST;
        return -1;
    }
    if (how == EPOLL_CTL_MOD) {
        assert(current->events);
        ++refreshes;
    } else assert(how == EPOLL_CTL_ADD);
    *current = *event;
    return 0;
}

int model_epoll_wait(int port, struct epoll_event *events, int max, int timeout) {
    assert(port == 21 && timeout == 0 && (unsigned)max >= waiting_count);
    memcpy(events, waiting, waiting_count * sizeof(*events));
    int n = (int)waiting_count;
    waiting_count = 0;
    return n;
}

static Ring *reset(unsigned slot_count) {
    free(pending.items);
    memset(&pending, 0, sizeof(pending));
    memset(rings, 0, sizeof(rings));
    memset(admissions, 0, sizeof(admissions));
    memset(connections, 0, sizeof(connections));
    memset(submissions, 0, sizeof(submissions));
    memset(watched, 0, sizeof(watched));
    closing_count = ordinary_pending = admission_used = admission_free = delayed_imports = 0;
    read_returned = read_allocated = waiting_count = refreshes = head = 0;
    ordinary_accept = named_imports = ring_watches = 1;
    slots = slot_count;
    Ring *r = &rings[0];
    r->conns = connections;
    r->parked_fd = 21;
    r->sq_head = &head;
    r->sq_mask = SQ_ENTRIES - 1;
    r->sqes = submissions;
    return r;
}

static unsigned admit(Ring *r, int fd, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        append_admission(r, fd + (int)i);
        ++r->accepting;
        ++ordinary_pending;
    }
    unsigned id = r->admission_batch;
    flush_admission(r);
    return id;
}

static void complete(Ring *r, uint64_t tag, int result) {
    struct io_uring_cqe event = {.user_data = tag, .res = result};
    on_cqe(r, &event);
}

static void edge(Ring *r, uint64_t k, unsigned events) {
    waiting[0] = (struct epoll_event){.events = events, .data.u64 = k};
    waiting_count = 1;
    assert(collect_bank(r, MODEL_SLOTS) == 1);
}

static void permuted_prefix(const unsigned order[4], unsigned done, int reject_watch) {
    const unsigned count = 3;
    Ring *r = reset(MODEL_SLOTS);
    unsigned id = admit(r, FIRST_FD, count);
    AdmissionBatch *b = &admissions[id];
    uint64_t keys[count];
    assert(r->tail == 4 && submissions[0].opcode == IORING_OP_FILES_UPDATE);
    assert(submissions[0].off == 0 && submissions[0].len == count);
    for (unsigned i = 0; i < count; ++i) {
        keys[i] = b->watches[i].data.u64;
        assert(!find_conn(keys[i]) && submissions[i + 1].opcode == IORING_OP_EPOLL_CTL);
        assert(submissions[i + 1].off == FIRST_FD + i);
        if (!(reject_watch && i == 1)) {
            watched[i] = b->watches[i];
            edge(r, keys[i], EPOLLIN | (i == 2 ? EPOLLRDHUP : 0));
            assert(connections[i].flags & EARLY_EDGE);
        }
    }
    assert(!pending.count && r->tail == 4);
    for (unsigned j = 0; j < 4; ++j) {
        unsigned which = order[j];
        int result = which == 0 ? (done ? (int)done : -EAGAIN) :
                     which == 2 && reject_watch ? -EOPNOTSUPP : 0;
        complete(r, submissions[which].user_data, result);
        if (j != 3) {
            assert(r->live == 0 && r->accepting == count && ordinary_pending == count);
            assert(!closing_count && !delayed_imports && r->named_inflight == 1);
        }
    }
    assert(!r->named_inflight && closing_count == done && r->live == done);
    assert(r->accepting == count - done && ordinary_pending == count);
    unsigned early = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (i < done) {
            assert(closing_alias[i] == FIRST_FD + (int)i);
            assert(find_conn(keys[i]) == &connections[i] && (connections[i].flags & PARKED));
            assert(watched[i].data.u64 == keys[i]);
            if (!(reject_watch && i == 1)) ++early;
            if (i == 2) assert(connections[i].flags & HANGUP);
        } else assert(!find_conn(keys[i]) && !connections[i].flags);
    }
    assert(pending.count == early && issue_park() == early && pending.inflight == early);
    assert(ring_watches == !reject_watch);
    if (done == count) {
        assert(admission_free == id && !delayed_imports);
        return;
    }
    assert(delayed_imports == id && r->kernel_allocates && b->count == count - done);
    b->retry_at = 0;
    retry_admissions();
    struct io_uring_sqe *retry = &submissions[r->tail - 1];
    assert(retry->opcode == IORING_OP_FILES_UPDATE && retry->off == IORING_FILE_INDEX_ALLOC);
    for (unsigned i = 0; i < b->count; ++i) {
        assert(b->indices[i] == FIRST_FD + (int)(done + i));
        b->indices[i] = (int)(done + i);
    }
    complete(r, retry->user_data, (int)b->count);
    assert(r->live == count && !r->accepting && !delayed_imports && closing_count == count);
    for (unsigned i = done; i < count; ++i) {
        uint64_t fresh = watched[i].data.u64;
        assert(fresh != keys[i] && !find_conn(keys[i]) && find_conn(fresh));
        unsigned tail = r->tail;
        edge(r, keys[i], EPOLLIN);
        assert(r->tail == tail);
        edge(r, fresh, EPOLLIN);
        assert(r->tail == tail + 1 && (find_conn(fresh)->flags & READING));
        edge(r, fresh, EPOLLIN);
        assert(r->tail == tail + 1 && (find_conn(fresh)->flags & REDO));
    }
    assert(refreshes == count - done - (unsigned)(reject_watch && done <= 1));
}

static void exhausted_bank(void) {
    Ring *r = reset(3);
    unsigned first = admit(r, FIRST_FD, 2);
    unsigned second = admit(r, FIRST_FD + 2, 2);
    assert(r->kernel_allocates && r->named_inflight == 1 && delayed_imports == second);
    assert(r->next_named_slot == 2 && r->tail == 3);
    complete(r, op(NAMED_IMPORT, first), 2);
    complete(r, op(NAMED_WATCH, (uint64_t)first * IMPORT_WIDTH), 0);
    assert(r->tail == 3 && !closing_count);
    complete(r, op(NAMED_WATCH, (uint64_t)first * IMPORT_WIDTH + 1), 0);
    assert(!r->named_inflight && closing_count == 2 && r->live == 2);
    admissions[second].retry_at = 0;
    retry_admissions();
    assert(r->tail == 4 && submissions[3].off == IORING_FILE_INDEX_ALLOC);
}

static void interleaved_batches(int unsupported) {
    Ring *r = reset(5);
    unsigned a = admit(r, FIRST_FD, 2);
    unsigned b = admit(r, FIRST_FD + 2, 2);
    unsigned c = admit(r, FIRST_FD + 4, 2);
    assert(admissions[a].first_slot == 0 && admissions[b].first_slot == 2);
    assert(r->named_inflight == 2 && r->kernel_allocates && delayed_imports == c);
    complete(r, op(NAMED_WATCH, b * IMPORT_WIDTH + 1), 0);
    complete(r, op(NAMED_WATCH, b * IMPORT_WIDTH), 0);
    complete(r, op(NAMED_IMPORT, b), 2);
    assert(r->named_inflight == 1 && r->live == 2 && closing_count == 2);
    assert(find_conn(admissions[b].watches[0].data.u64));
    assert(!find_conn(admissions[a].watches[0].data.u64));
    admissions[c].retry_at = 0;
    retry_admissions();
    assert(delayed_imports == c && r->tail == 6);
    complete(r, op(NAMED_IMPORT, a), unsupported ? -EOPNOTSUPP : 2);
    complete(r, op(NAMED_WATCH, a * IMPORT_WIDTH + 1), 0);
    complete(r, op(NAMED_WATCH, a * IMPORT_WIDTH), 0);
    assert(!r->named_inflight && r->live == (unsupported ? 2u : 4u));
    assert(named_imports == !unsupported && closing_count == r->live);
    admissions[c].retry_at = 0;
    if (unsupported) {
        assert(delayed_imports == a && admissions[a].next == c);
        admissions[a].retry_at = 0;
    }
    retry_admissions();
    assert(!delayed_imports && r->tail == (unsupported ? 8u : 7u));
    for (unsigned i = 6; i < r->tail; ++i) {
        assert(submissions[i].opcode == IORING_OP_FILES_UPDATE);
        assert(submissions[i].off == IORING_FILE_INDEX_ALLOC);
    }
}

static unsigned check_orders(unsigned order[4], unsigned length, unsigned used) {
    if (length == 4) {
        for (unsigned installed = 0; installed <= 3; ++installed)
            for (int rejected = 0; rejected <= 1; ++rejected)
                permuted_prefix(order, installed, rejected);
        return 8;
    }
    unsigned cases = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned bit = 1u << i;
        if (used & bit) continue;
        order[length] = i;
        cases += check_orders(order, length + 1, used | bit);
    }
    return cases;
}

int main(void) {
    unsigned order[4];
    unsigned cases = check_orders(order, 0, 0);
    exhausted_bank();
    interleaved_batches(0);
    interleaved_batches(1);
    free(pending.items);
    printf("%u completion-order cases and three bank-handoff cases passed\n", cases);
    return 0;
}
