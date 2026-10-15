cdef void wheel_remove(Server* s, Connection* c) noexcept nogil:
    if c.bucket < 0:
        return
    if c.prev >= 0:
        s.connections[c.prev].next = c.next
    else:
        s.wheel[c.bucket] = c.next
    if c.next >= 0:
        s.connections[c.next].prev = c.prev
    c.bucket = -1

cdef void refresh_deadline(Server* s, Connection* c) noexcept nogil:
    cdef uint64_t deadline = mono_ms() // 1000 + IDLE_SECONDS
    # Read and immediate send usually land in the same timer bucket.
    if c.deadline == deadline:
        return
    wheel_remove(s, c)
    c.deadline = deadline
    c.bucket = c.deadline % WHEEL_SIZE
    c.prev = -1
    c.next = s.wheel[c.bucket]
    if c.next >= 0:
        s.connections[c.next].prev = c.fd
    s.wheel[c.bucket] = c.fd

cdef void close_connection(Server* s, Connection* c) noexcept nogil:
    if c.fd < 0:
        return
    wheel_remove(s, c)
    epoll_ctl(s.epfd, EPOLL_CTL_DEL, c.fd, NULL)
    close(c.fd)
    buf_release(&s.pool, &c.input)
    output_release(&s.pool, &c.output)
    request_release(&s.pool, c.request)
    c.request = NULL
    c.fd = -1
    c.flags = 0

cdef void read_request(Server* s, Connection* c) noexcept nogil:
    cdef ssize_t n
    # One recv per turn bounds work per connection and preserves scheduler fairness.
    if c.input.len >= INPUT_CAP or buf_reserve(&s.pool, &c.input, c.input.len + 4096) != 0:
        close_connection(s, c)
        return
    n = recv(c.fd, c.input.data + c.input.len, min(c.input.cap - c.input.len, 4096), 0)
    if n == 0:
        close_connection(s, c)
        return
    if n < 0:
        if not is_again() and not is_interrupted():
            close_connection(s, c)
        return
    c.input.len += n
    refresh_deadline(s, c)
    if parse_input(s, c) != 0:
        close_connection(s, c)
    # Ready requests remain read-registered until output needs backpressure.

cdef void accept_connections(Server* s) noexcept nogil:
    cdef int fd, i
    cdef Connection* c
    for i in range(128):
        fd = next_client(s.listener)
        if fd < 0: return
        if fd >= FD_CAP:
            close(fd)
            continue
        c = &s.connections[fd]
        memset(c,0,sizeof(Connection))
        c.fd = fd
        s.generation += 1
        c.generation = s.generation & 0xffffffffULL
        c.prev = c.next = c.bucket = -1
        c.parser = &s.parsers[fd]
        llhttp_init(c.parser,HTTP_REQUEST,&s.settings)
        parser_set_data(c.parser,c)
        if register_fd(s,c) != 0:
            close_connection(s,c)
            continue
        refresh_deadline(s,c)

cdef int run_loop(Server* s) noexcept nogil:
    cdef epoll_event events[128]
    cdef uint64_t tag, now
    cdef int count, i, fd, nxt
    cdef Connection* c
    while running:
        count = epoll_wait(s.epfd,events,128,0 if s.head != s.tail else 250)
        if count < 0:
            if is_interrupted(): continue
            return -1
        for i in range(count):
            tag = event_tag(&events[i])
            if tag == 0xffffffffffffffffULL:
                accept_connections(s)
                continue
            fd = <uint32_t>tag
            if fd >= FD_CAP: continue
            c = &s.connections[fd]
            if c.fd < 0 or c.generation != tag >> 32: continue
            if events[i].events & (EPOLLERR | EPOLLHUP):
                close_connection(s,c)
                continue
            if events[i].events & EPOLLIN and not c.flags & CF_READY:
                read_request(s,c)
            if c.fd >= 0 and events[i].events & EPOLLOUT:
                flush_response(s,c)
            # A half-close must drain buffered input and queued output before closing.
            if c.fd >= 0 and events[i].events & EPOLLRDHUP:
                c.flags |= CF_PEER_CLOSED
                if set_events(s,c,(EPOLLOUT if c.flags & CF_WRITE else 0) if c.flags & CF_READY else EPOLLIN) != 0: close_connection(s,c)
        for i in range(64):
            if s.head == s.tail: break
            tag = s.ready[s.head % READY_CAP]
            s.head += 1
            fd = <uint32_t>tag
            c = &s.connections[fd]
            if c.fd < 0 or c.generation != tag >> 32: continue
            if build_response(s,c) != 0:
                close_connection(s,c)
            else:
                flush_response(s,c)
        now = mono_ms() // 1000
        while s.tick < now:
            s.tick += 1
            fd = s.wheel[s.tick % WHEEL_SIZE]
            while fd >= 0:
                c = &s.connections[fd]
                nxt = c.next
                if c.deadline <= s.tick: close_connection(s,c)
                fd = nxt
    return 0
