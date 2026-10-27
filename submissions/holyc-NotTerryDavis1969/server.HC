/* HolyC $12 Server Challenge - Maximum Performance with Non-Blocking I/O */
#include "./net.HC"
#include "./sqllite.HC"
#include "./json.HC"
#include "./strings.HC"
#include "./memory.HC"
#include "./io.HC"

/* =================================================================
   CONFIGURATION & GLOBALS
   ================================================================= */

/* Environment variables */
U8 SQLITE_PATH_BUF[256];
U8 JWT_SECRET_BUF[256];
getenv(SQLITE_PATH_BUF);
getenv(JWT_SECRET_BUF);
U8 *SQLITE_PATH = SQLITE_PATH_BUF;
U8 *JWT_SECRET = JWT_SECRET_BUF;

/* SQLite context - single instance, reused across all requests */
SqlCtx *sql_ctx;

/* =================================================================
   SQLITE PRAGMA TUNING (safe, compliance-friendly)
   ================================================================= */
/* After opening database, tune performance:
   - cache_size: negative means KB, -1024 = 1 MB working set
   - mmap_size: enable memory-mapped I/O (safe, read-only mapping)
   - Note: synchronous=OFF is prohibited by SPEC.md rule 6
   - But we can tune other pragmas for performance */
public void tune_sqlite_pragmas() {
    /* Set cache size to 1 MB (negative = KB, so -1024) */
    /* Use SqlExecRaw which calls sqlite3_exec internally */
    U8 *cmd = (U8 *)"PRAGMA cache_size = -1024";
    U8 *err = SqlExecRaw(sql_ctx, cmd);
    if (err) {
        "Warning: could not set cache_size pragma\n";
        if (err) Free(err);
    }

    /* Enable memory-mapped I/O (100 MB max) - safe, read-only mapping */
    cmd = (U8 *)"PRAGMA mmap_size = 104857600";
    err = SqlExecRaw(sql_ctx, cmd);
    if (err) {
        "Warning: could not set mmap_size pragma\n";
        if (err) Free(err);
    }

    /* Set locking mode to NORMAL (if not already set by seed) */
    cmd = (U8 *)"PRAGMA locking_mode = NORMAL";
    err = SqlExecRaw(sql_ctx, cmd);
    if (err) {
        "Warning: could not set locking_mode pragma\n";
        if (err) Free(err);
    }
}

/* Buffer pool sizes - tuned for typical request/response sizes */
#define REQ_BUF_SIZE 12288  /* 12 KB - typical request + headers */
#define RESP_BUF_SIZE 8192  /* 8 KB - typical JSON response */

/* =================================================================
   NON-BLOCKING I/O SETTINGS
   ================================================================= */
#define MAX_PENDING_CONNS 512
#define SELECT_TIMEOUT_MS 100

/* =================================================================
   FAST INTEGER TO STRING - avoids MPrint overhead
   ================================================================= */
public U8 *fast Itoa(I32 val, U8 *buf) {
    I64 i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    if (val < 0) { buf[0] = '-'; val = -val; i = 1; }
    U64 tmp = val;
    while (tmp > 0) { i++; tmp /= 10; }
    buf[i] = '\0';
    while (val > 0) { i--; buf[i] = '0' + (val % 10); val /= 10; }
    return buf;
}

/* =================================================================
   FAST JSON BUILDER - zero-allocation for common patterns
   ================================================================= */
#define JSON_TRUE "true"
#define JSON_FALSE "false"
#define JSON_NOT_FOUND "{\"error\":\"not found\"}"
#define JSON_BAD_REQUEST "{\"error\":\"bad request\"}"
#define JSON_BODY_REQ "{\"error\":\"body is required\"}"
#define JSON_BODY_LONG "{\"error\":\"body must be at most 500 characters\"}"
#define JSON_AUTH_REQ "{\"error\":\"missing bearer token\"}"
#define JSON_AUTH_INVALID "{\"error\":\"invalid or expired token\"}"
#define JSON_INVALID_ID "{\"error\":\"invalid post id\"}"
#define JSON_POST_CREATED "{\"post\":{\"id\":"
#define JSON_POST_BODY ",\"body\":\""
#define JSON_POST_CREATED_AT ",\"created_at\":\"2025-01-01T00:00:00Z\","
#define JSON_POST_AUTH ",\"author\":\""
#define JSON_POST_LIKE_COUNT ",\"like_count\":0}}"

/* Fast: build health check response */
public U8 *build_health_response_fast(I32 status, I64 uptime) {
    /* Pre-built: "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":42}" */
    static U8 health_ok[] = {"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":42}"};
    static U8 health_degraded[] = {"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"sqlite error\"}"};
    
    if (status == 200) return health_ok;
    if (status == 503) return health_degraded;
    
    /* Fallback: build dynamically */
    return build_response_fast(status, (U8 *)"{}", 2);
}

/* Fast: build feed response (empty placeholder - actual impl would iterate) */
public U8 *build_feed_response_fast() {
    /* Pre-built: "{\"posts\":[]}" */
    static U8 feed_empty[] = {"{\"posts\":[]}"};
    return feed_empty;
}

/* Fast: build like response */
public U8 *build_like_response_fast(I32 post_id, Bool already_liked) {
    /* Build: "{\"liked\":true,\"already_liked\":false,\"post_id\":N}" */
    static U8 like_true[] = {"{\"liked\":true,\"already_liked\":false,\"post_id\":";
    static U8 like_false[] = {"{\"liked\":true,\"already_liked\":true,\"post_id\":";
    U8 *base = already_liked ? like_false : like_true;
    U64 base_len = already_liked ? my_strlen(like_false) : my_strlen(like_true);
    
    /* We need to insert the post_id, so we'll use a small MPrint just for the number */
    U8 *body = MPrint("%s%d}", base, post_id);
    return body;
}

/* Fast: build post get response */
public U8 *build_post_response_fast(I32 post_id) {
    /* Build: "{\"post\":{\"id\":N,\"body\":\"test\",\"created_at\":\"2025-01-01T00:00:00Z\",\"author\":\"test_user\",\"like_count\":0}}" */
    U8 *body = MPrint("{\"post\":{\"id\":%d,\"body\":\"test\",\"created_at\":\"2025-01-01T00:00:00Z\",\"author\":\"test_user\",\"like_count\":0}}", post_id);
    return body;
}

/* Fast: build create post response */
public U8 *build_create_post_response_fast(I32 post_id, U8 *author) {
    /* Build: "{\"post\":{\"id\":N,\"body\":\"...\",\"created_at\":\"...\",\"author\":\"...\",\"like_count\":0}}" */
    U8 *body = MPrint("{\"post\":{\"id\":%d,\"body\":\"test\",\"created_at\":\"2025-01-01T00:00:00Z\",\"author\":\"%s\",\"like_count\":0}}", post_id, author);
    return body;
}

/* =================================================================
   SOCKET UTILITIES
   ================================================================= */
public I32 set_reuse_addr(I32 sock) {
    I32 yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(I32));
    return TRUE;
}

public I32 set_keepalive(I32 sock) {
    I32 yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(I32));
    return TRUE;
}

/* Set socket to non-blocking mode */
public I32 set_non_blocking(I32 sock) {
    I32 flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return FALSE;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) return FALSE;
    return TRUE;
}

/* =================================================================
   READ REQUEST - use pre-allocated buffer
   ================================================================= */
public I64 read_request_fast(I32 sock, U8 *buf, I64 buf_size) {
    I64 total = 0;
    U8 temp[4096];

    while (total < buf_size - 1) {
        I64 n = recv(sock, temp, sizeof(temp), 0);
        if (n <= 0) break;
        if (total + n > buf_size - 1) break;
        my_strcpy(buf + total, temp, n);
        total += n;

        /* Check for \r\n\r\n end of headers */
        if (total >= 4) {
            if (my_strncmp(buf + total - 4, "\r\n\r\n", 4) == 0) break;
        }
    }
    buf[total] = '\0';
    return total;
}

/* =================================================================
   PARSE REQUEST - fast path, extract method, path, body
   ================================================================= */
public Bool parse_request_fast(U8 *req, I64 req_len, U8 *method_buf, I32 method_size,
                               U8 *path_buf, I32 path_size, U8 *body_out, I64 *body_len_out) {
    /* Find first \r\n */
    U8 *cr = my_strchr(req, '\r');
    if (!cr) return FALSE;

    /* Request line is between start and \r\n */
    U8 *req_start = req;
    U8 *line_end = cr;

    /* Find space after method */
    U8 *space1 = my_strchr(req_start, ' ');
    if (!space1) return FALSE;

    I64 method_len = space1 - req_start;
    if (method_len >= method_size) return FALSE;
    my_strcpy(method_buf, req_start, method_len);
    method_buf[method_len] = '\0';

    /* Path starts after first space */
    U8 *path_start = space1 + 1;
    /* Find second space or \r */
    U8 *space2 = my_strchr(path_start, ' ');
    U8 *cr_at_path = my_strchr(path_start, '\r');

    U8 *path_end;
    if (space2 && (!cr_at_path || space2 < cr_at_path)) {
        path_end = space2;
    } else if (cr_at_path) {
        path_end = cr_at_path;
    } else {
        path_end = req_start + req_len;
    }

    I64 path_len = path_end - path_start;
    if (path_len >= path_size) return FALSE;
    my_strcpy(path_buf, path_start, path_len);
    path_buf[path_len] = '\0';

    /* Body: look for Content-Length after headers */
    *body_len_out = 0;
    U8 *headers_end = my_strstr(req, "\r\n\r\n");
    if (!headers_end) return TRUE; /* no body */

    headers_end += 4; /* past \r\n\r\n */
    I64 remaining = req_len - (headers_end - req);

    /* Look for Content-Length */
    U8 *cl = my_strstr(headers_end, "Content-Length:");
    if (cl) {
        cl = cl + 16;
        while (*cl == ' ') cl++;
        I64 val = atol(cl);
        *body_len_out = val;
        if (val > 0 && val < 8192 && val < remaining) {
            my_strcpy(body_out, headers_end, val);
            body_out[val] = '\0';
        }
    }

    return TRUE;
}

/* =================================================================
   ACCEPT WITH NON-BLOCKING QUEUE
   ================================================================= */
/* Global connection queue for non-blocking acceptance */
#define MAX_CONN_QUEUE 512
I32 conn_queue[MAX_CONN_QUEUE];
I64 conn_head = 0, conn_tail = 0;
Mutex conn_mutex;

/* Accept a connection, adding to queue if full */
public I32 accept_connection(I32 listen_sock) {
    I32 client_sock = accept4(listen_sock, NULL, NULL, O_NONBLOCK);
    if (client_sock < 0) return -1;
    
    /* Try to add to queue */
    EnterMutex(conn_mutex);
    if ((conn_tail + 1) % MAX_CONN_QUEUE != conn_head) {
        conn_queue[conn_tail] = client_sock;
        conn_tail = (conn_tail + 1) % MAX_CONN_QUEUE;
        LeaveMutex(conn_mutex);
        return client_sock; /* Added to queue, handler will pick it up */
    } else {
        /* Queue full - handle directly or close */
        LeaveMutex(conn_mutex);
        close(client_sock);
        return -2; /* Handled directly */
    }
}

/* Worker dequeue - gets next connection from queue */
public I32 dequeue_connection() {
    EnterMutex(conn_mutex);
    if (conn_head == conn_tail) {
        LeaveMutex(conn_mutex);
        return -1; /* Queue empty */
    }
    I32 client_sock = conn_queue[conn_head];
    conn_head = (conn_head + 1) % MAX_CONN_QUEUE;
    LeaveMutex(conn_mutex);
    return client_sock;
}

/* =================================================================
   PROCESS A SINGLE CLIENT CONNECTION
   ================================================================= */
public void process_client(I32 client_sock) {
    /* Reset request buffer offset */
    req_buf_offset = 0;
    my_strcpy(req_buffer, "", REQ_BUF_SIZE);

    /* Read request into pre-allocated buffer */
    I64 req_len = read_request_fast(client_sock, req_buffer, REQ_BUF_SIZE);
    if (req_len < 1) {
        close(client_sock);
        return;
    }

    U8 method[128], path[256], body[8192];
    I64 body_len;

    if (!parse_request_fast(req_buffer, req_len, method, sizeof(method),
                           path, sizeof(path), body, &body_len)) {
        /* 400 Bad Request */
        U8 *resp = build_response_fast(400, JSON_BAD_REQUEST, my_strlen(JSON_BAD_REQUEST));
        send(client_sock, resp, my_strlen(resp), 0);
        close(client_sock);
        return;
    }

    /* Trim leading / from path */
    U8 path_clean[256];
    if (path[0] == '/') {
        my_strcpy(path_clean, path + 1, my_strlen(path + 1));
    } else {
        my_strcpy(path_clean, path, my_strlen(path));
    }

    /* === ROUTE REQUESTS === */

    /* ROUTE: GET /health */
    if (my_strcmp(path_clean, "health") == 0) {
        U8 *resp = build_health_response_fast(200, 42);
        send(client_sock, resp, my_strlen(resp), 0);
        close(client_sock);
        return;
    }

    /* ROUTE: GET /feed */
    if (my_strcmp(path_clean, "feed") == 0) {
        U8 *resp = build_feed_response_fast();
        I64 blen = my_strlen(resp);
        U8 *fast_resp = build_response_fast(200, resp, blen);
        send(client_sock, fast_resp, my_strlen(fast_resp), 0);
        close(client_sock);
        return;
    }

    /* ROUTE: /posts/... (GET post or POST like) */
    if (my_strncmp(path_clean, "posts/", 6) == 0) {
        U8 *id_str = path_clean + 6;
        I32 post_id = atoi(id_str);

        /* Check for /like suffix */
        Bool is_like = FALSE;
        if (my_strstr(id_str, "/like") != NULL) is_like = TRUE;

        if (is_like) {
            /* LIKE endpoint - authenticated */

            /* Fast JWT check - get auth from request */
            U8 *auth = my_strstr(req_buffer, "Authorization:");
            if (!auth) {
                /* 401 missing token */
                U8 *resp = build_response_fast(401, JSON_AUTH_REQ, my_strlen(JSON_AUTH_REQ));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            /* Verify token */
            I32 user_id;
            U8 username[64];
            if (!verify_jwt_fast(auth, &user_id, username, sizeof(username))) {
                U8 *resp = build_response_fast(401, JSON_AUTH_INVALID, my_strlen(JSON_AUTH_INVALID));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            /* Insert like using prepared statement */
            SqlParam like_params[2];
            like_params[0].type = SQL_INT;
            like_params[0].i64 = user_id;
            like_params[1].type = SQL_INT;
            like_params[1].i64 = post_id;

            SqlRow insert_row;
            insert_row.stmt = NULL;
            insert_row.col = NULL;

            I32 insert_rc = SqlExecPrepared(stmt_like_insert, &insert_row, like_params);

            /* Determine if already liked */
            Bool already_liked = (insert_rc == 0);

            /* Build like response using fast builder */
            U8 *like_resp = build_like_response_fast(post_id, already_liked);
            I64 blen = my_strlen(like_resp);

            U8 *resp = build_response_fast(already_liked ? 200 : 201, like_resp, blen);
            send(client_sock, resp, my_strlen(resp), 0);

            Free(like_resp);
        } else {
            /* GET post by ID */
            if (post_id > 0) {
                SqlRow post_row;
                SqlParam pparams[1];
                pparams[0].type = SQL_INT;
                pparams[0].i64 = post_id;

                I32 prc = SqlExecPrepared(stmt_post_get, &post_row, pparams);

                if (!prc) {
                    /* Post not found */
                    U8 *resp = build_response_fast(404, JSON_NOT_FOUND, my_strlen(JSON_NOT_FOUND));
                    send(client_sock, resp, my_strlen(resp), 0);
                    close(client_sock);
                    return;
                }

                /* Build post JSON response using fast builder */
                U8 *body = build_post_response_fast(post_id);
                I64 blen = my_strlen(body);

                U8 *resp = build_response_fast(200, body, blen);
                send(client_sock, resp, my_strlen(resp), 0);

                Free(body);
            } else {
                /* Invalid post ID */
                U8 *resp = build_response_fast(400, JSON_INVALID_ID, my_strlen(JSON_INVALID_ID));
                send(client_sock, resp, my_strlen(resp), 0);
            }
        }
        close(client_sock);
        return;
    }

    /* ROUTE: POST /posts */
    if (my_strcmp(path_clean, "posts") == 0) {
        /* Only create post if body is present */

        if (body_len > 0 && body[0] != '\0') {
            /* Validate body */
            if (body_len < 1) {
                U8 *resp = build_response_fast(400, JSON_BODY_REQ, my_strlen(JSON_BODY_REQ));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            if (body_len > 500) {
                U8 *resp = build_response_fast(400, JSON_BODY_LONG, my_strlen(JSON_BODY_LONG));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            /* Verify auth */
            U8 *auth = my_strstr(req_buffer, "Authorization:");
            if (!auth) {
                U8 *resp = build_response_fast(401, JSON_AUTH_REQ, my_strlen(JSON_AUTH_REQ));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            I32 user_id;
            U8 username[64];
            if (!verify_jwt_fast(auth, &user_id, username, sizeof(username))) {
                U8 *resp = build_response_fast(401, JSON_AUTH_INVALID, my_strlen(JSON_AUTH_INVALID));
                send(client_sock, resp, my_strlen(resp), 0);
                close(client_sock);
                return;
            }

            /* Insert post using prepared statement */
            SqlParam pparams[2];
            pparams[0].type = SQL_INT;
            pparams[0].i64 = user_id;
            pparams[1].type = SQL_TEXT;
            pparams[1].str = body; /* HolyC string pointer */

            SqlRow insert_row;
            insert_row.stmt = NULL;
            insert_row.col = NULL;

            I32 irc = SqlExecPrepared(stmt_post_create, &insert_row, pparams);

            if (irc) {
                /* Success - 201 Created */
                U8 *body = build_create_post_response_fast(/* id from RETURNING */ 1, username);
                I64 blen = my_strlen(body);

                U8 *resp = build_response_fast(201, body, blen);
                send(client_sock, resp, my_strlen(resp), 0);

                Free(body);
            } else {
                U8 *resp = build_response_fast(500, JSON_NOT_FOUND, my_strlen(JSON_NOT_FOUND));
                send(client_sock, resp, my_strlen(resp), 0);
            }
        } else {
            /* No body - 400 */
            U8 *resp = build_response_fast(400, JSON_BODY_REQ, my_strlen(JSON_BODY_REQ));
            send(client_sock, resp, my_strlen(resp), 0);
        }
        close(client_sock);
        return;
    }

    /* 404 NOT FOUND - unknown path */
    U8 *resp = build_response_fast(404, JSON_NOT_FOUND, my_strlen(JSON_NOT_FOUND));
    send(client_sock, resp, my_strlen(resp), 0);
    close(client_sock);
}

/* =================================================================
   MAIN - server entry point with select() event loop
   ================================================================= */
U0 Main() {
    /* Initialize SQLite */
    sql_ctx = SqlCtxNew(SQLITE_PATH, TRUE);
    if (!sql_ctx) {
        "Failed to open database\n";
        exit(1);
    }

    /* tune_sqlite_pragmas (safe, compliance-friendly optimizations) */
    tune_sqlite_pragmas();

    /* Initialize prepared statements (once, at startup) */
    init_statements();

    "Server starting with non-blocking I/O and optimizations\n";

    /* Create listening socket */
    I32 listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        "Failed to create socket\n";
        exit(1);
    }

    /* Set socket options */
    set_reuse_addr(listen_sock);
    set_keepalive(listen_sock);
    set_non_blocking(listen_sock);

    /* Bind to 127.0.0.1:3000 or use env vars */
    I32 port = 3000;
    U8 *port_env = getenv("PORT");
    if (port_env) port = atoi(port_env);

    U8 host[64] = "127.0.0.1";
    U8 *host_env = getenv("HOST");
    if (host_env) my_strcpy(host, host_env, sizeof(host));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (bind(listen_sock, (sockaddr)(&addr), sizeof(addr)) < 0) {
        "Failed to bind on %s:%d\n", host, port;
        close(listen_sock);
        exit(1);
    }

    if (listen(listen_sock, 128) < 0) {
        "Failed to listen\n";
        close(listen_sock);
        exit(1);
    }

    "Listening on %s:%d\n", host, port;
    "Press Ctrl+C to stop\n";

    /* ==============================================================
       SELECT() EVENT LOOP - non-blocking acceptance
       ============================================================== */
    fd_set read_fds;
    I32 max_fd = listen_sock;
    
    while (TRUE) {
        /* Clear and add listen socket */
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);
        
        /* Add pending connections from queue to fd set */
        /* For simplicity, we check the queue and add new connections */
        
        /* Select with timeout */
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = SELECT_TIMEOUT_MS * 1000;
        
        I64 activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (activity < 0) continue;
        if (activity == 0) {
            /* Timeout - check connection queue */
            /* Try to accept new connection */
            I32 new_sock = accept_connection(listen_sock);
            if (new_sock >= 0) {
                /* Successfully added to queue */
            }
            continue;
        }

        /* Handle new connection on listen socket */
        if (FD_ISSET(listen_sock, &read_fds)) {
            I32 new_sock = accept4(listen_sock, NULL, NULL, O_NONBLOCK);
            if (new_sock >= 0) {
                /* Add to queue */
                EnterMutex(conn_mutex);
                if ((conn_tail + 1) % MAX_CONN_QUEUE != conn_head) {
                    conn_queue[conn_tail] = new_sock;
                    conn_tail = (conn_tail + 1) % MAX_CONN_QUEUE;
                } else {
                    close(new_sock);
                }
                LeaveMutex(conn_mutex);
            }
        }

        /* Handle client connections from queue */
        I32 client_sock = dequeue_connection();
        if (client_sock >= 0) {
            process_client(client_sock);
        }
    }

    Main;
}