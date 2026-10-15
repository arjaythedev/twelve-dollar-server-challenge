cdef int on_begin(llhttp_t* parser) noexcept nogil:
    cdef Connection* c = <Connection*>parser_data(parser)
    c.request = request_acquire(&active_server.pool)
    if c.request == NULL:
        return -1
    request_reset(c.request)
    return 0

cdef int on_url(llhttp_t* parser, const char* data, size_t n) noexcept nogil:
    cdef Request* r = (<Connection*>parser_data(parser)).request
    if n > sizeof(r.url) - r.url_len:
        return -1
    memcpy(r.url + r.url_len, data, n)
    r.url_len += n
    return 0

cdef int on_field(llhttp_t* parser, const char* data, size_t n) noexcept nogil:
    cdef Request* r = (<Connection*>parser_data(parser)).request
    cdef size_t i
    cdef unsigned char ch
    if r.header_value:
        r.header_len = 0
        r.header_value = 0
    if n > sizeof(r.header) - r.header_len:
        return -1
    for i in range(n):
        ch = <unsigned char>data[i]
        r.header[r.header_len + i] = ch + 32 if ch >= 65 and ch <= 90 else ch
    r.header_len += n
    return 0

cdef int on_value(llhttp_t* parser, const char* data, size_t n) noexcept nogil:
    cdef Request* r = (<Connection*>parser_data(parser)).request
    if not r.header_value:
        r.header_value = 1
        r.header_auth = r.header_len == 13 and memcmp(r.header, "authorization", 13) == 0
        if r.header_auth:
            if r.auth_seen:
                return -1
            r.auth_seen = 1
    if r.header_auth:
        if n > AUTH_CAP - r.auth.len:
            return -1
        if r.auth.data == NULL and buf_reserve(&active_server.pool,&r.auth,AUTH_CAP) != 0:
            return -1
        if buf_append(&active_server.pool, &r.auth, data, n) != 0:
            return -1
    return 0

cdef int on_headers(llhttp_t* parser) noexcept nogil:
    (<Connection*>parser_data(parser)).request.method = llhttp_get_method(parser)
    return 0

cdef int on_body(llhttp_t* parser, const char* data, size_t n) noexcept nogil:
    cdef Request* r = (<Connection*>parser_data(parser)).request
    if n > BODY_CAP - r.body.len:
        return -1
    if r.body.data == NULL and buf_reserve(&active_server.pool,&r.body,BODY_CAP) != 0:
        return -1
    if buf_append(&active_server.pool, &r.body, data, n) != 0:
        return -1
    return 0

cdef int on_complete(llhttp_t* parser) noexcept nogil:
    cdef Connection* c = <Connection*>parser_data(parser)
    cdef Server* s = active_server
    if s.tail - s.head >= READY_CAP:
        return -1
    if not llhttp_should_keep_alive(parser):
        c.flags |= CF_CLOSE
    c.flags |= CF_READY
    s.ready[s.tail % READY_CAP] = (c.generation << 32) | <uint32_t>c.fd
    s.tail += 1
    return HPE_PAUSED

cdef int parse_input(Server* s, Connection* c) noexcept nogil:
    cdef int result
    cdef const char* start
    if c.parsed >= c.input.len:
        return 0
    start = c.input.data + c.parsed
    result = llhttp_execute(c.parser, start, c.input.len - c.parsed)
    if result == HPE_PAUSED:
        c.parsed += llhttp_get_error_pos(c.parser) - start
        return 0
    if result != HPE_OK:
        return -1
    c.parsed = c.input.len
    # Request-owned fragments survive releasing consumed input.
    buf_release(&s.pool, &c.input)
    c.parsed = 0
    return 0

cdef int build_response(Server* s, Connection* c) noexcept nogil:
    cdef Writer body
    cdef char headers[256]
    cdef int status, n
    cdef const char* reason
    memset(&body,0,sizeof(Writer))
    if writer_reset(&s.pool,&body,RESPONSE_HEADROOM) != 0:
        return -1
    status = dispatch(s,c,&body)
    if status < 0:
        writer_release(&s.pool,&body)
        return -1
    reason = "OK"
    if status == 201: reason = "Created"
    elif status == 400: reason = "Bad Request"
    elif status == 401: reason = "Unauthorized"
    elif status == 404: reason = "Not Found"
    elif status == 500: reason = "Internal Server Error"
    elif status == 503: reason = "Service Unavailable"
    n = snprintf(
        headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %u\r\nConnection: %s\r\n\r\n",
        status, reason, body.len-RESPONSE_HEADROOM,
        "close" if c.flags & CF_CLOSE else "keep-alive"
    )
    if n <= 0 or n > RESPONSE_HEADROOM or n >= <int>sizeof(headers):
        writer_release(&s.pool,&body)
        return -1
    # Headers occupy reserved chunk space; partial sends retain chunk ownership.
    memcpy(body.head.data+RESPONSE_HEADROOM-n,headers,n)
    output_take(&c.output,&body,RESPONSE_HEADROOM-n)
    request_release(&s.pool,c.request)
    c.request = NULL
    return 0

cdef void flush_response(Server* s, Connection* c) noexcept nogil:
    cdef ssize_t n
    if c.output.len:
        n = output_send(&s.pool,c.fd,&c.output)
        if n < 0:
            if is_again() or is_interrupted():
                if set_events(s,c,EPOLLOUT) != 0: close_connection(s,c)
            else:
                close_connection(s,c)
            return
        if n > 0: refresh_deadline(s,c)
    if c.output.len:
        if set_events(s,c,EPOLLOUT) != 0: close_connection(s,c)
        return
    output_release(&s.pool,&c.output)
    if c.flags & CF_CLOSE:
        close_connection(s,c)
        return
    c.flags &= ~(CF_READY | CF_CLOSE)
    llhttp_resume(c.parser)
    if c.input.len > c.parsed:
        # Resume at parsed; pipelined bytes remain in their original buffer.
        if parse_input(s,c) != 0:
            close_connection(s,c)
            return
    else:
        buf_release(&s.pool,&c.input)
        c.parsed = 0
    if set_events(s,c,0 if c.flags & CF_READY else EPOLLIN) != 0: close_connection(s,c)

cdef void configure_parser(Server* s) noexcept nogil:
    llhttp_settings_init(&s.settings)
    s.settings.on_message_begin = on_begin
    s.settings.on_url = on_url
    s.settings.on_header_field = on_field
    s.settings.on_header_value = on_value
    s.settings.on_headers_complete = on_headers
    s.settings.on_body = on_body
    s.settings.on_message_complete = on_complete
