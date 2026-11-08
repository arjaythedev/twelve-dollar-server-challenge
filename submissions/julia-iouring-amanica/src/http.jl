# HTTP/1.1 request parsing in place over the connection's read buffer (no copies, no Strings).

const M_OTHER = 0
const M_GET = 1
const M_POST = 2

mutable struct Req
    method::Int
    ps::Int; pe::Int        # path, query string stripped
    as::Int; ae::Int        # Authorization value (as == 0: absent)
    bs::Int; bl::Int        # body start, length
    keepalive::Bool
    next::Int               # index just past this request
end
Req() = Req(0, 0, 0, 0, 0, 0, 0, true, 0)

const MAX_HEADER = 65536

@inline lower(c::UInt8) = (0x41 <= c <= 0x5a) ? c + 0x20 : c

# d[s:e] equals the lowercase literal, ignoring ASCII case.
function ieq(d::Vector{UInt8}, s::Int, e::Int, lit::String)
    e - s + 1 == sizeof(lit) || return false
    @inbounds for k in 0:e-s
        lower(d[s+k]) == codeunit(lit, k + 1) || return false
    end
    true
end

function icontains(d::Vector{UInt8}, s::Int, e::Int, lit::String)
    n = sizeof(lit)
    for i in s:e-n+1
        ieq(d, i, i + n - 1, lit) && return true
    end
    false
end

@inline function bytes_eq(d::Vector{UInt8}, s::Int, e::Int, lit::String)
    e - s + 1 == sizeof(lit) || return false
    @inbounds for k in 0:e-s
        d[s+k] == codeunit(lit, k + 1) || return false
    end
    true
end

@inline function bytes_startswith(d::Vector{UInt8}, s::Int, e::Int, lit::String)
    e - s + 1 >= sizeof(lit) && bytes_eq(d, s, s + sizeof(lit) - 1, lit)
end

# Parses one request starting at d[pos], with data up to d[len].
# Returns 1 (complete, fields in r), 0 (need more bytes) or -1 (malformed).
function parse_req!(r::Req, d::Vector{UInt8}, pos::Int, len::Int)
    hend = 0                 # index of the final '\n' of "\r\n\r\n"
    i = pos
    @inbounds while i + 3 <= len
        if d[i+3] == 0x0a && d[i+2] == 0x0d && d[i+1] == 0x0a && d[i] == 0x0d
            hend = i + 3
            break
        end
        i += 1
    end
    if hend == 0
        return len - pos + 1 > MAX_HEADER ? -1 : 0
    end

    # request line: METHOD SP target SP HTTP/1.x CRLF
    i = pos
    @inbounds while i <= hend && d[i] != UInt8(' ')
        i += 1
    end
    r.method = bytes_eq(d, pos, i - 1, "GET") ? M_GET : bytes_eq(d, pos, i - 1, "POST") ? M_POST : M_OTHER
    ts = i + 1
    i = ts
    @inbounds while i <= hend && d[i] != UInt8(' ') && d[i] != 0x0d
        i += 1
    end
    te = i - 1
    (i <= hend && @inbounds(d[i]) == UInt8(' ')) || return -1
    vs = i + 1
    bytes_eq(d, vs, vs + 7, "HTTP/1.1") || bytes_eq(d, vs, vs + 7, "HTTP/1.0") || return -1
    http11 = @inbounds d[vs+7] == UInt8('1')
    r.ps = ts
    q = ts
    @inbounds while q <= te && d[q] != UInt8('?')
        q += 1
    end
    r.pe = q - 1
    i = vs + 8
    @inbounds (d[i] == 0x0d && d[i+1] == 0x0a) || return -1
    i += 2

    r.as = 0; r.ae = 0
    clen = 0
    conn_close = false
    conn_keep = false
    while i < hend - 1            # one header line per iteration; the last CRLF is the blank line
        ls = i
        @inbounds while d[i] != 0x0d
            i += 1
        end
        le = i - 1
        i += 2
        c = ls
        @inbounds while c <= le && d[c] != UInt8(':')
            c += 1
        end
        c <= le || return -1
        v = c + 1
        @inbounds while v <= le && (d[v] == UInt8(' ') || d[v] == 0x09)
            v += 1
        end
        ve = le
        @inbounds while ve >= v && (d[ve] == UInt8(' ') || d[ve] == 0x09)
            ve -= 1
        end
        nlen = c - ls
        if nlen == 14 && ieq(d, ls, c - 1, "content-length")
            v <= ve || return -1
            clen = 0
            @inbounds for k in v:ve
                isdigitb(d[k]) || return -1
                clen = clen * 10 + Int(d[k] - 0x30)
                clen > (1 << 24) && return -1
            end
        elseif nlen == 13 && ieq(d, ls, c - 1, "authorization")
            r.as = v; r.ae = ve
        elseif nlen == 10 && ieq(d, ls, c - 1, "connection")
            icontains(d, v, ve, "close") && (conn_close = true)
            icontains(d, v, ve, "keep-alive") && (conn_keep = true)
        elseif nlen == 17 && ieq(d, ls, c - 1, "transfer-encoding")
            return -1                 # chunked request bodies are not supported
        end
    end
    hend + clen <= len || return 0
    r.bs = hend + 1
    r.bl = clen
    r.keepalive = http11 ? !conn_close : conn_keep
    r.next = hend + clen + 1
    1
end

status_line(s::Int) =
    s == 200 ? "HTTP/1.1 200 OK\r\n" :
    s == 201 ? "HTTP/1.1 201 Created\r\n" :
    s == 400 ? "HTTP/1.1 400 Bad Request\r\n" :
    s == 401 ? "HTTP/1.1 401 Unauthorized\r\n" :
    s == 404 ? "HTTP/1.1 404 Not Found\r\n" :
    s == 503 ? "HTTP/1.1 503 Service Unavailable\r\n" :
    "HTTP/1.1 500 Internal Server Error\r\n"

function write_response!(out::Buf, status::Int, body::Buf, keepalive::Bool)
    putstr!(out, status_line(status))
    putstr!(out, "Content-Type: application/json\r\nContent-Length: ")
    putint!(out, body.len)
    keepalive || putstr!(out, "\r\nConnection: close")
    putstr!(out, "\r\n\r\n")
    putbuf!(out, body)
end
