# HTTP/1.1 request parsing: request line, the three headers the API needs,
# Content-Length body. Everything else is skipped.
from bytes import Bytes, eqlit, eqlit_ci, find_byte, find_crlf, parse_uint

comptime M_OTHER = 0
comptime M_GET = 1
comptime M_POST = 2

comptime P_INCOMPLETE = 0
comptime P_COMPLETE = 1
comptime P_BAD = 2


struct Req:
    var status: Int
    var method: Int
    var path: Bytes
    var path_len: Int
    var auth_present: Bool   # any Authorization header
    var bearer: Bool         # it starts with "Bearer "
    var tok: Bytes
    var tok_len: Int
    var body: Bytes
    var body_len: Int
    var close: Bool
    var consumed: Int        # total bytes of this request

    def __init__(out self, p: Bytes):
        self.status = P_INCOMPLETE
        self.method = M_OTHER
        self.path = p
        self.path_len = 0
        self.auth_present = False
        self.bearer = False
        self.tok = p
        self.tok_len = 0
        self.body = p
        self.body_len = 0
        self.close = False
        self.consumed = 0


def parse_request(p: Bytes, n: Int, mut r: Req):
    r.status = P_INCOMPLETE
    # request line
    var eol = find_crlf(p, n, 0)
    if eol < 0:
        if n > 8192:
            r.status = P_BAD
        return
    var sp1 = find_byte(p, eol, 0, 0x20)
    var sp2 = find_byte(p, eol, sp1 + 1, 0x20) if sp1 >= 0 else -1
    if sp1 < 0 or sp2 < 0:
        r.status = P_BAD
        return
    if eqlit(p, sp1, "GET"):
        r.method = M_GET
    elif eqlit(p, sp1, "POST"):
        r.method = M_POST
    else:
        r.method = M_OTHER
    r.path = p.unsafe_offset(sp1 + 1)
    r.path_len = sp2 - sp1 - 1
    # drop a query string
    var qm = find_byte(r.path, r.path_len, 0, 0x3F)
    if qm >= 0:
        r.path_len = qm
    var vlen = eol - sp2 - 1
    var http10 = eqlit(p.unsafe_offset(sp2 + 1), vlen, "HTTP/1.0")
    if not http10 and not eqlit(p.unsafe_offset(sp2 + 1), vlen, "HTTP/1.1"):
        r.status = P_BAD
        return
    r.close = http10
    r.auth_present = False
    r.bearer = False
    r.body_len = 0
    var content_length: Int64 = 0
    var i = eol + 2
    while True:
        var e = find_crlf(p, n, i)
        if e < 0:
            if n > 8192:
                r.status = P_BAD
            return
        if e == i:
            i += 2
            break
        var colon = -1
        for k in range(i, e):
            if p[unsafe_offset=k] == 0x3A:
                colon = k
                break
        if colon < 0:
            r.status = P_BAD
            return
        var name_len = colon - i
        var vs = colon + 1
        while vs < e and (p[unsafe_offset=vs] == 0x20 or p[unsafe_offset=vs] == 0x09):
            vs += 1
        var ve = e
        while ve > vs and (p[unsafe_offset=ve - 1] == 0x20 or p[unsafe_offset=ve - 1] == 0x09):
            ve -= 1
        var name = p.unsafe_offset(i)
        if name_len == 13 and eqlit_ci(name, 13, "authorization"):
            r.auth_present = True
            if ve - vs >= 7 and eqlit_ci(p.unsafe_offset(vs), 7, "bearer "):
                r.bearer = True
                var ts = vs + 7
                while ts < ve and p[unsafe_offset=ts] == 0x20:
                    ts += 1
                r.tok = p.unsafe_offset(ts)
                r.tok_len = ve - ts
        elif name_len == 14 and eqlit_ci(name, 14, "content-length"):
            content_length = parse_uint(p.unsafe_offset(vs), ve - vs)
            if content_length < 0:
                r.status = P_BAD
                return
        elif name_len == 10 and eqlit_ci(name, 10, "connection"):
            if eqlit_ci(p.unsafe_offset(vs), ve - vs, "close"):
                r.close = True
            elif eqlit_ci(p.unsafe_offset(vs), ve - vs, "keep-alive"):
                r.close = False
        elif name_len == 17 and eqlit_ci(name, 17, "transfer-encoding"):
            r.status = P_BAD
            return
        i = e + 2
    if content_length > 16384:
        r.status = P_BAD
        return
    var cl = Int(content_length)
    if n - i < cl:
        return
    r.body = p.unsafe_offset(i)
    r.body_len = cl
    r.consumed = i + cl
    r.status = P_COMPLETE
