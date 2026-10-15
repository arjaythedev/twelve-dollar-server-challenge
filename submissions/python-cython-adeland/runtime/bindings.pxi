cdef extern from "native.h" nogil:
    ctypedef long ssize_t
    enum:
        FD_CAP
        INPUT_CAP
        AUTH_CAP
        BODY_CAP
        RESPONSE_HEADROOM
        YYJSON_READ_INSITU
        YYJSON_PADDING_SIZE
        READY_CAP
        WHEEL_SIZE
        IDLE_SECONDS
        CF_READY
        CF_CLOSE
        CF_WRITE
        CF_PEER_CLOSED
        ROUTE_NOT_FOUND
        ROUTE_HEALTH
        ROUTE_FEED
        ROUTE_POST
        ROUTE_CREATE
        ROUTE_LIKE
        EPOLLIN
        EPOLLOUT
        EPOLLERR
        EPOLLHUP
        EPOLLRDHUP
        EPOLL_CTL_ADD
        EPOLL_CTL_DEL
        SQLITE_OK
        SQLITE_ROW
        SQLITE_DONE
        SQLITE_OPEN_READWRITE
        SQLITE_OPEN_NOMUTEX
        HTTP_REQUEST
        HTTP_GET
        HTTP_POST
        HPE_OK
        HPE_PAUSED
    ctypedef struct Pool:
        pass
    ctypedef struct Buffer:
        char* data
        uint32_t len
        uint32_t cap
    ctypedef struct Chunk:
        char data[4032]
    ctypedef struct Writer:
        Chunk* head
        Chunk* tail
        uint32_t len
    ctypedef struct Output:
        uint32_t len
    ctypedef struct Request:
        uint32_t url_len
        uint32_t header_len
        uint8_t auth_seen
        uint8_t header_auth
        uint8_t header_value
        uint8_t method
        char url[1024]
        char header[64]
        Buffer auth
        Buffer body
    ctypedef struct llhttp_t:
        pass
    ctypedef struct llhttp_settings_t:
        int (*on_message_begin)(llhttp_t*) noexcept nogil
        int (*on_url)(llhttp_t*, const char*, size_t) noexcept nogil
        int (*on_header_field)(llhttp_t*, const char*, size_t) noexcept nogil
        int (*on_header_value)(llhttp_t*, const char*, size_t) noexcept nogil
        int (*on_headers_complete)(llhttp_t*) noexcept nogil
        int (*on_body)(llhttp_t*, const char*, size_t) noexcept nogil
        int (*on_message_complete)(llhttp_t*) noexcept nogil
    ctypedef struct Connection:
        int fd
        uint32_t flags
        uint64_t deadline
        uint64_t generation
        Buffer input
        Output output
        uint32_t parsed
        int prev
        int next
        int bucket
        llhttp_t* parser
        Request* request
    ctypedef struct sqlite3:
        pass
    ctypedef struct sqlite3_stmt:
        pass
    ctypedef struct yyjson_alc:
        pass
    ctypedef struct yyjson_doc:
        pass
    ctypedef struct yyjson_val:
        pass
    ctypedef struct Server:
        int epfd
        int listener
        int wheel[128]
        Connection* connections
        llhttp_t* parsers
        llhttp_settings_t settings
        # native.h owns the array extent; Cython only indexes its decayed pointer.
        uint64_t* ready
        uint64_t head
        uint64_t tail
        uint64_t generation
        uint64_t start_ms
        uint64_t tick
        Pool pool
        sqlite3* db
        sqlite3_stmt* feed
        sqlite3_stmt* post
        sqlite3_stmt* create
        sqlite3_stmt* like
        sqlite3_stmt* exists
        sqlite3_stmt* health
        char json_arena[524288]
        char decoded_header[8192]
        char decoded_payload[16384]
        char decoded_signature[64]
        yyjson_alc json_allocator
    cdef struct epoll_event:
        uint32_t events
    int posix_memalign(void**, size_t, size_t)
    uint64_t mono_ms()
    void pool_destroy(Pool*)
    void request_reset(Request*)
    Request* request_acquire(Pool*)
    void request_release(Pool*, Request*)
    Server* server_new()
    size_t connection_request_offset()
    size_t request_prefix_bytes()
    int buf_reserve(Pool*, Buffer*, size_t)
    int buf_append(Pool*, Buffer*, const char*, size_t)
    int writer_literal(Pool*, Writer*, const char*)
    int writer_int64(Pool*, Writer*, int64_t)
    int writer_string(Pool*, Writer*, const char*, size_t)
    int writer_reset(Pool*, Writer*, uint32_t)
    void writer_release(Pool*, Writer*)
    void output_release(Pool*, Output*)
    ssize_t output_send(Pool*, int, Output*)
    void output_take(Output*, Writer*, uint32_t)
    void buf_release(Pool*, Buffer*)
    int set_events(Server*, Connection*, uint32_t)
    int register_fd(Server*, Connection*)
    int make_listener(const char*, int)
    int next_client(int)
    int is_again()
    int is_interrupted()
    uint64_t event_tag(epoll_event*)
    void set_listener_event(epoll_event*)
    void* parser_data(llhttp_t*)
    void parser_set_data(llhttp_t*, void*)
    Server* active_server
    int running
    void install_signals()
    int hmac_verify(Server*, const char*, size_t, const char*)
    int crypto_init(Server*, const char*, size_t)
    void crypto_destroy(Server*)
    int close(int)
    ssize_t recv(int, void*, size_t, int)
    int epoll_create1(int)
    int epoll_ctl(int, int, int, epoll_event*)
    int epoll_wait(int, epoll_event*, int, int)
    void llhttp_settings_init(llhttp_settings_t*)
    void llhttp_init(llhttp_t*, int, const llhttp_settings_t*)
    int llhttp_execute(llhttp_t*, const char*, size_t)
    const char* llhttp_get_error_pos(llhttp_t*)
    int llhttp_get_method(llhttp_t*)
    int llhttp_should_keep_alive(llhttp_t*)
    void llhttp_resume(llhttp_t*)
    int sqlite3_open_v2(const char*, sqlite3**, int, const char*)
    int sqlite3_close(sqlite3*)
    int sqlite3_exec(sqlite3*, const char*, void*, void*, char**)
    int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**)
    int sqlite3_bind_int64(sqlite3_stmt*, int, int64_t)
    int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void*)
    int sqlite3_step(sqlite3_stmt*)
    int sqlite3_reset(sqlite3_stmt*)
    int sqlite3_clear_bindings(sqlite3_stmt*)
    int sqlite3_finalize(sqlite3_stmt*)
    int64_t sqlite3_column_int64(sqlite3_stmt*, int)
    const unsigned char* sqlite3_column_text(sqlite3_stmt*, int)
    int sqlite3_column_bytes(sqlite3_stmt*, int)
    int sqlite3_changes(sqlite3*)
    const char* sqlite3_errmsg(sqlite3*)
    int yyjson_alc_pool_init(yyjson_alc*, void*, size_t)
    yyjson_doc* yyjson_read_opts(char*, size_t, uint32_t, const yyjson_alc*, void*)
    void yyjson_doc_free(yyjson_doc*)
    yyjson_val* yyjson_doc_get_root(yyjson_doc*)
    yyjson_val* yyjson_obj_get(yyjson_val*, const char*)
    int yyjson_is_obj(yyjson_val*)
    int yyjson_is_str(yyjson_val*)
    int yyjson_is_num(yyjson_val*)
    const char* yyjson_get_str(yyjson_val*)
    size_t yyjson_get_len(yyjson_val*)
    double yyjson_get_num(yyjson_val*)
