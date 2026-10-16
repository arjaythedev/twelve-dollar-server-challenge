cdef int error_body(Server* s, Writer* b, const char* message) noexcept nogil:
    if writer_reset(&s.pool,b,RESPONSE_HEADROOM) != 0: return -1
    return writer_literal(&s.pool, b, "{\"error\":") or writer_string(&s.pool, b, message, strlen(message)) or writer_literal(&s.pool, b, "}")

cdef int write_post(Server* s, Writer* b, sqlite3_stmt* stmt) noexcept nogil:
    cdef const char* text
    cdef int n
    if (
        writer_literal(&s.pool,b,"{\"id\":")
        or writer_int64(&s.pool,b,sqlite3_column_int64(stmt,0))
        or writer_literal(&s.pool,b,",\"body\":")
    ):
        return -1
    text = <const char*>sqlite3_column_text(stmt,1)
    n = sqlite3_column_bytes(stmt,1)
    if text == NULL or writer_string(&s.pool,b,text,n) or writer_literal(&s.pool,b,",\"created_at\":"): return -1
    text = <const char*>sqlite3_column_text(stmt,2)
    n = sqlite3_column_bytes(stmt,2)
    if text == NULL or writer_string(&s.pool,b,text,n) or writer_literal(&s.pool,b,",\"author\":"): return -1
    text = <const char*>sqlite3_column_text(stmt,3)
    n = sqlite3_column_bytes(stmt,3)
    if (
        text == NULL
        or writer_string(&s.pool,b,text,n)
        or writer_literal(&s.pool,b,",\"like_count\":")
        or writer_int64(&s.pool,b,sqlite3_column_int64(stmt,4))
        or writer_literal(&s.pool,b,"}")
    ):
        return -1
    return 0

cdef int read_posts(Server* s, Writer* b, int feed, int64_t pid) noexcept nogil:
    cdef sqlite3_stmt* stmt = s.feed if feed else s.post
    cdef int rc, count = 0, bad = 0
    if not feed and sqlite3_bind_int64(stmt, 1, pid) != SQLITE_OK:
        reset_statement(stmt)
        return 500
    if writer_literal(&s.pool,b,"{\"posts\":[" if feed else "{\"post\":"):
        reset_statement(stmt)
        return 500
    rc = sqlite3_step(stmt)
    while rc == SQLITE_ROW:
        if count and writer_literal(&s.pool,b,","):
            bad = 1
            break
        if write_post(s,b,stmt):
            bad = 1
            break
        count += 1
        rc = sqlite3_step(stmt)
    if rc != SQLITE_DONE or bad:
        reset_statement(stmt)
        return 500
    reset_statement(stmt)
    if not feed and count == 0:
        return 404
    if writer_literal(&s.pool,b,"]}" if feed else "}"):
        return 500
    return 200

cdef int create_post(Server* s, Request* r, Writer* b, Auth* auth, const char** error) noexcept nogil:
    cdef yyjson_doc* doc = read_json(s,r.body.data,r.body.len,r.body.cap)
    cdef yyjson_val* val
    cdef const char* body
    cdef const char* created
    cdef size_t start, end
    cdef int chars, rc, bad = 0
    cdef int64_t pid
    if doc == NULL:
        error[0] = "malformed JSON body"
        return 400
    val = yyjson_obj_get(yyjson_doc_get_root(doc), "body")
    if not yyjson_is_str(val):
        yyjson_doc_free(doc)
        error[0] = "body is required"
        return 400
    body = yyjson_get_str(val)
    chars = trim_body(body, yyjson_get_len(val), &start, &end)
    if chars <= 0 or chars > 500:
        yyjson_doc_free(doc)
        error[0] = "body is required" if chars <= 0 else "body must be at most 500 characters"
        return 400
    body += start
    if sqlite3_bind_int64(s.create,1,auth.user_id) != SQLITE_OK or sqlite3_bind_text(s.create,2,body,end-start,NULL) != SQLITE_OK:
        reset_statement(s.create)
        yyjson_doc_free(doc)
        return 500
    rc = sqlite3_step(s.create)
    if rc != SQLITE_ROW:
        reset_statement(s.create)
        yyjson_doc_free(doc)
        return 500
    pid = sqlite3_column_int64(s.create,0)
    created = <const char*>sqlite3_column_text(s.create,1)
    if created == NULL:
        bad = 1
    elif writer_literal(&s.pool,b,"{\"post\":{\"id\":") or writer_int64(&s.pool,b,pid) or writer_literal(&s.pool,b,",\"body\":") or writer_string(&s.pool,b,body,end-start) or writer_literal(&s.pool,b,",\"created_at\":") or writer_string(&s.pool,b,created,sqlite3_column_bytes(s.create,1)) or writer_literal(&s.pool,b,",\"author\":") or writer_string(&s.pool,b,auth.username,auth.username_len) or writer_literal(&s.pool,b,",\"like_count\":0}}"):
        bad = 1
    # Step RETURNING to DONE and reset before exposing a committed response.
    rc = sqlite3_step(s.create)
    if reset_statement(s.create) != SQLITE_OK:
        bad = 1
    yyjson_doc_free(doc)
    return 201 if rc == SQLITE_DONE and not bad else 500

cdef int like_post(Server* s, Writer* b, Auth* auth, int64_t pid) noexcept nogil:
    cdef int rc, changes
    if sqlite3_bind_int64(s.like,1,auth.user_id) != SQLITE_OK or sqlite3_bind_int64(s.like,2,pid) != SQLITE_OK:
        reset_statement(s.like)
        return 500
    rc = sqlite3_step(s.like)
    changes = sqlite3_changes(s.db)
    if reset_statement(s.like) != SQLITE_OK or rc != SQLITE_DONE:
        return 500
    if changes == 0:
        sqlite3_bind_int64(s.exists,1,pid)
        rc = sqlite3_step(s.exists)
        reset_statement(s.exists)
        if rc == SQLITE_DONE:
            return 404
        if rc != SQLITE_ROW:
            return 500
    if writer_literal(&s.pool,b,"{\"liked\":true,\"already_liked\":") or writer_literal(&s.pool,b,"false" if changes else "true") or writer_literal(&s.pool,b,",\"post_id\":") or writer_int64(&s.pool,b,pid) or writer_literal(&s.pool,b,"}"):
        return 500
    return 201 if changes else 200

cdef int dispatch(Server* s, Connection* c, Writer* b) noexcept nogil:
    cdef Request* r = c.request
    cdef size_t n = r.url_len, i, id_end
    cdef int route = ROUTE_NOT_FOUND, status = 200, rc
    cdef int64_t pid = 0
    cdef const char* error = "internal server error"
    cdef Auth auth
    memset(&auth,0,sizeof(Auth))
    for i in range(n):
        if r.url[i] == 63:
            n = i
            break
    if r.method == HTTP_GET:
        if n == 7 and memcmp(r.url,"/health",7) == 0: route = ROUTE_HEALTH
        elif n == 5 and memcmp(r.url,"/feed",5) == 0: route = ROUTE_FEED
        elif n > 7 and memcmp(r.url,"/posts/",7) == 0: route = ROUTE_POST
    elif r.method == HTTP_POST:
        if n == 6 and memcmp(r.url,"/posts",6) == 0: route = ROUTE_CREATE
        elif n > 12 and memcmp(r.url,"/posts/",7) == 0 and memcmp(r.url+n-5,"/like",5) == 0: route = ROUTE_LIKE
    if route >= ROUTE_CREATE:
        yyjson_alc_pool_init(&s.json_allocator,s.json_arena,sizeof(s.json_arena))
    if route == ROUTE_NOT_FOUND:
        status = 404
        error = "not found"
    elif route >= ROUTE_CREATE:
        rc = authenticate(s,r,&auth,&error)
        if rc != 0: status = 401
    if status == 200 and (route == ROUTE_POST or route == ROUTE_LIKE):
        id_end = n - 5 if route == ROUTE_LIKE else n
        if parse_positive_id(r.url+7,id_end-7,&pid) != 0:
            status = 400
            error = "invalid post id"
    if status == 200:
        if route == ROUTE_HEALTH:
            rc = sqlite3_step(s.health)
            reset_statement(s.health)
            if rc != SQLITE_ROW:
                status = 503
                if (
                    writer_literal(&s.pool,b,"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
                    or writer_string(&s.pool,b,sqlite3_errmsg(s.db),strlen(sqlite3_errmsg(s.db)))
                    or writer_literal(&s.pool,b,"}")
                ):
                    status = 500
            elif (
                writer_literal(&s.pool,b,"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
                or writer_int64(&s.pool,b,(mono_ms()-s.start_ms)//1000)
                or writer_literal(&s.pool,b,"}")
            ):
                status = 500
        elif route == ROUTE_FEED or route == ROUTE_POST:
            status = read_posts(s,b,route == ROUTE_FEED,pid)
            if status == 404: error = "post not found"
        elif route == ROUTE_CREATE:
            status = create_post(s,r,b,&auth,&error)
        elif route == ROUTE_LIKE:
            status = like_post(s,b,&auth,pid)
            if status == 404: error = "post not found"
    yyjson_doc_free(auth.doc)
    if status == 500:
        error = "internal server error"
    if status >= 400 and status != 503:
        if error_body(s,b,error): return -1
    return status
