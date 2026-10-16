# cython: language_level=3
from libc.stdint cimport uint8_t, uint32_t, uint64_t, int64_t
from libc.stdlib cimport calloc, free
from libc.string cimport memcpy, memset, memcmp, strlen
from libc.stdio cimport snprintf
from libc.time cimport time

include "runtime/bindings.pxi"
include "runtime/validation.pxi"
include "runtime/auth.pxi"
include "runtime/database.pxi"
include "runtime/handlers.pxi"
include "runtime/http.pxi"
include "runtime/reactor.pxi"

def layout():
    return {'connection_bytes': sizeof(Connection), 'connection_alignment': 64,
            'parser_bytes': sizeof(llhttp_t), 'request_bytes': sizeof(Request),
            'request_pool_block_bytes': 4096,
            'request_prefix_bytes': request_prefix_bytes(),
            'connection_request_offset': connection_request_offset(),
            'connection_slots': FD_CAP, 'pool_limit_bytes': 128*1024*1024,
            'ready_ring_bytes': READY_CAP*8, 'json_arena_bytes': 524288}

def serve(bytes path, bytes secret, bytes host, int port):
    global active_server, running
    cdef Server* s
    cdef epoll_event event
    cdef int i, result = -1
    cdef const char* dbpath = path
    cdef const char* key = secret
    cdef const char* bind_host = host
    if active_server != NULL:
        raise RuntimeError('a reactor is already running')
    if port < 1 or port > 65535:
        raise ValueError('port must be between 1 and 65535')
    s = server_new()
    if s == NULL: raise MemoryError()
    try:
        if posix_memalign(<void**>&s.connections,64,sizeof(Connection)*FD_CAP) != 0:
            raise MemoryError()
        memset(s.connections,0,sizeof(Connection)*FD_CAP)
        for i in range(FD_CAP):
            s.connections[i].fd = -1
            s.connections[i].bucket = -1
        s.parsers = <llhttp_t*>calloc(FD_CAP,sizeof(llhttp_t))
        if s.parsers == NULL: raise MemoryError()
        for i in range(WHEEL_SIZE): s.wheel[i] = -1
        open_database(s,dbpath)
        if crypto_init(s,key,len(secret)) != 0:
            raise RuntimeError('cannot initialize HMAC')
        configure_parser(s)
        s.listener = make_listener(bind_host,port)
        if s.listener < 0: raise RuntimeError('cannot bind listening socket')
        s.epfd = epoll_create1(0)
        if s.epfd < 0: raise RuntimeError('cannot create epoll')
        set_listener_event(&event)
        if epoll_ctl(s.epfd,EPOLL_CTL_ADD,s.listener,&event) != 0:
            raise RuntimeError('cannot register listening socket')
        active_server = s
        running = 1
        install_signals()
        s.start_ms = mono_ms()
        s.tick = s.start_ms // 1000
        with nogil:
            result = run_loop(s)
    finally:
        if s.connections != NULL:
            for i in range(FD_CAP):
                if s.connections[i].fd >= 0: close_connection(s,&s.connections[i])
        if s.listener >= 0: close(s.listener)
        if s.epfd >= 0: close(s.epfd)
        close_database(s)
        crypto_destroy(s)
        free(s.parsers)
        free(s.connections)
        pool_destroy(&s.pool)
        free(s)
        active_server = NULL
    if result != 0: raise RuntimeError('reactor failed')
