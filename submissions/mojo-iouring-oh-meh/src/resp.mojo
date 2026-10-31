# Response arenas. A batch writes its responses back to back into an arena; each
# send points into it (no copy) and holds a reference until its completion
# arrives. An arena is reused once nothing in flight points into it.
from bytes import Bytes, balloc, bcopy, lit, Out

comptime HDR_RESERVE = 128
comptime ARENA_SIZE = 1 << 20

# Upper bounds for the variable-size responses, so an arena is reserved tightly:
# a post object is at most ~3 KB (500 code points escaped as \u00XX) plus a
# username; the feed is 20 of them; a created post echoes a token username.
comptime POST_MAX = 32 << 10
comptime FEED_MAX = 96 << 10
comptime CREATE_MAX = 64 << 10


@fieldwise_init
struct Arena(ImplicitlyCopyable, Copyable, Movable):
    var base: Int        # address
    var used: Int
    var inflight: Int


@fieldwise_init
struct Resp(ImplicitlyCopyable, Copyable, Movable):
    var ptr: Int         # address of the first byte to send
    var len: Int
    var arena: Int       # index, or -1 for a constant response


def status_line(status: Int) -> StringSpan[ImmStaticOrigin]:
    if status == 200:
        return "HTTP/1.1 200 OK\r\n"
    if status == 201:
        return "HTTP/1.1 201 Created\r\n"
    if status == 400:
        return "HTTP/1.1 400 Bad Request\r\n"
    if status == 401:
        return "HTTP/1.1 401 Unauthorized\r\n"
    if status == 404:
        return "HTTP/1.1 404 Not Found\r\n"
    if status == 413:
        return "HTTP/1.1 413 Content Too Large\r\n"
    if status == 503:
        return "HTTP/1.1 503 Service Unavailable\r\n"
    return "HTTP/1.1 500 Internal Server Error\r\n"


# Write the header for a body of body_len ending exactly at base[body_start].
# Returns the header's start offset. `scratch` is HDR_RESERVE bytes of workspace.
def write_header(base: Bytes, body_start: Int, status: Int, body_len: Int, scratch: Bytes) -> Int:
    var tmp = Out(scratch, HDR_RESERVE)
    tmp.put_lit(status_line(status))
    tmp.put_lit("Content-Type: application/json\r\nContent-Length: ")
    tmp.put_int(Int64(body_len))
    tmp.put_lit("\r\n\r\n")
    var start = body_start - tmp.n
    bcopy(base.unsafe_offset(start), tmp.p, tmp.n)
    return start


# A constant response, built once, never freed.
def const_resp(status: Int, body: StringSpan[ImmStaticOrigin]) -> Resp:
    var n = body.byte_length()
    var base = balloc(HDR_RESERVE + n)
    bcopy(base.unsafe_offset(HDR_RESERVE), lit(body), n)
    var start = write_header(base, HDR_RESERVE, status, n, balloc(HDR_RESERVE))
    return Resp(Int(base) + start, HDR_RESERVE + n - start, -1)


struct Arenas:
    var arenas: List[Arena]
    var cur: Int
    var hdr_scratch: Bytes

    def __init__(out self):
        self.arenas = List[Arena]()
        self.arenas.append(Arena(Int(balloc(ARENA_SIZE)), 0, 0))
        self.cur = 0
        self.hdr_scratch = balloc(HDR_RESERVE)

    # Make sure the current arena has room for `need` more bytes.
    def ensure(mut self, need: Int):
        if self.arenas[self.cur].used + need <= ARENA_SIZE:
            return
        for i in range(len(self.arenas)):
            if i != self.cur and self.arenas[i].inflight == 0:
                self.arenas[i].used = 0
                self.cur = i
                return
        self.arenas.append(Arena(Int(balloc(ARENA_SIZE)), 0, 0))
        self.cur = len(self.arenas) - 1

    # Start a response body; returns an Out positioned at the body start.
    def begin(mut self, max_body: Int) -> Out:
        self.ensure(HDR_RESERVE + max_body)
        var a = self.arenas[self.cur]
        return Out(Bytes(unsafe_from_address=a.base + a.used + HDR_RESERVE), max_body)

    # Finish it: write the header just before the body, account the send.
    def finish(mut self, status: Int, o: Out) -> Resp:
        var base = Bytes(unsafe_from_address=self.arenas[self.cur].base)
        var body_start = Int(o.p) - self.arenas[self.cur].base
        var start = write_header(base, body_start, status, o.n, self.hdr_scratch)
        self.arenas[self.cur].used = body_start + o.n
        self.arenas[self.cur].inflight += 1
        return Resp(self.arenas[self.cur].base + start, body_start + o.n - start, self.cur)

    @always_inline
    def release(mut self, arena: Int):
        if arena >= 0:
            self.arenas[arena].inflight -= 1
