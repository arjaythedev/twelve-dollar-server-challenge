# Typed wrappers over the shim's io_uring entry points, so call sites read like Mojo.
from std.ffi import c_int, external_call
from bytes import Bytes

# CQE flag bits (linux/io_uring.h)
comptime CQE_F_BUFFER: UInt32 = 1
comptime CQE_F_MORE: UInt32 = 2
comptime CQE_BUFFER_SHIFT: UInt32 = 16

comptime ENOBUFS = -105
comptime ENFILE = -23
comptime EAGAIN = -11


@fieldwise_init
struct Cqe(ImplicitlyCopyable, Copyable, Movable):
    var res: Int32
    var flags: UInt32
    var user_data: UInt64


@always_inline
def table_size() -> Int:
    return Int(external_call["sh_table_size", UInt32]())


@always_inline
def ring_init(sq: Int, table: Int, nbufs: Int, bufsize: Int) -> Int:
    return Int(external_call["sh_init", c_int](UInt32(sq), UInt32(table), UInt32(nbufs), UInt32(bufsize)))


@always_inline
def ring_open(i: Int) -> Int:
    return Int(external_call["sh_ring_open", c_int](c_int(i)))


@always_inline
def buf_base(i: Int) -> Bytes:
    return Bytes(unsafe_from_address=external_call["sh_buf_base", Int](c_int(i)))


@always_inline
def buf_return(i: Int, bid: Int):
    external_call["sh_buf_return", NoneType](c_int(i), UInt32(bid))


@always_inline
def buf_flush(i: Int) -> Int:
    return Int(external_call["sh_buf_flush", c_int](c_int(i)))


@always_inline
def listen(host: String, port: Int, backlog: Int) -> Int:
    var h = host
    return Int(external_call["sh_listen", c_int](h.as_c_string_span(), c_int(port), c_int(backlog)))


@always_inline
def submit(i: Int) -> Int:
    return Int(external_call["sh_submit", c_int](c_int(i)))


@always_inline
def wait_one(i: Int) -> Int:
    return Int(external_call["sh_wait_one", UInt32](c_int(i)))


@always_inline
def wait(timeout_ms: Int) -> Int:
    return Int(external_call["sh_wait", UInt32](c_int(timeout_ms)))


@always_inline
def reap(i: Int, cqes: Pointer[Cqe, MutUntrackedOrigin], max: Int) -> Int:
    return Int(external_call["sh_reap", UInt32](c_int(i), cqes, UInt32(max)))


@always_inline
def prep_accept(i: Int, lfd: Int, ud: UInt64) -> Int:
    return Int(external_call["sh_prep_accept", c_int](c_int(i), c_int(lfd), ud))


@always_inline
def prep_recv(i: Int, slot: Int, ud: UInt64) -> Int:
    return Int(external_call["sh_prep_recv", c_int](c_int(i), c_int(slot), ud))


@always_inline
def prep_send(i: Int, slot: Int, buf: Int, n: Int, ud: UInt64) -> Int:
    return Int(external_call["sh_prep_send", c_int](c_int(i), c_int(slot), buf, UInt32(n), ud))


@always_inline
def prep_cancel(i: Int, target: UInt64, ud: UInt64) -> Int:
    return Int(external_call["sh_prep_cancel", c_int](c_int(i), target, ud))


@always_inline
def prep_close(i: Int, slot: Int, ud: UInt64) -> Int:
    return Int(external_call["sh_prep_close", c_int](c_int(i), c_int(slot), ud))


@always_inline
def now_ms() -> Int:
    return Int(external_call["sh_now_ms", Int64]())


@always_inline
def unix_time() -> Int64:
    return external_call["sh_unix_time", Int64]()


@always_inline
def mem_available_kb() -> Int:
    return Int(external_call["sh_mem_available_kb", Int64]())


@always_inline
def calloc_bytes(n: Int) -> Bytes:
    return Bytes(unsafe_from_address=external_call["sh_calloc", Int](n))


@always_inline
def malloc_bytes(n: Int) -> Int:
    return external_call["sh_malloc", Int](n)


@always_inline
def free_addr(p: Int):
    external_call["sh_free", NoneType](p)


def getenv(name: String, default: String) -> String:
    var k = name
    var p = external_call["sh_getenv", Int](k.as_c_string_span())
    if p == 0:
        return default
    return String(unsafe_from_utf8_ptr=Bytes(unsafe_from_address=p))


def log(msg: String):
    var m = msg
    external_call["sh_log", NoneType](m.as_c_string_span())
