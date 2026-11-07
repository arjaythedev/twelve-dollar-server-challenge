# Routing and the five endpoints. Each handler fills the response body buffer and returns the status.

const START = Ref(0.0)
const TXT = Buf(2048)       # decoded body of POST /posts
const BKEY = Buf(64)
const BF = Fields()

@inline function err!(b::Buf, status::Int, msg::String)
    reset!(b)
    putstr!(b, "{\"error\":\"")
    putstr!(b, msg)
    putstr!(b, "\"}")
    status
end

# Positive decimal integer in d[s:e], or -1.
function parse_id(d::Vector{UInt8}, s::Int, e::Int)
    (1 <= e - s + 1 <= 18) || return -1
    v = 0
    @inbounds for i in s:e
        c = d[i]
        isdigitb(c) || return -1
        v = v * 10 + Int(c - 0x30)
    end
    v > 0 ? v : -1
end

function has_slash(d::Vector{UInt8}, s::Int, e::Int)
    @inbounds for i in s:e
        d[i] == UInt8('/') && return true
    end
    false
end

# (status, user_id) from the Authorization header.
function authenticate(d::Vector{UInt8}, r::Req)
    r.as == 0 && return (AUTH_MISSING, 0)
    bytes_startswith(d, r.as, r.ae, "Bearer ") || return (AUTH_MISSING, 0)
    s = r.as + 7
    @inbounds while s <= r.ae && d[s] == UInt8(' ')
        s += 1
    end
    s <= r.ae || return (AUTH_INVALID, 0)
    verify_jwt(d, s, r.ae, time())
end

function auth_error!(b::Buf, st::Int)
    st == AUTH_MISSING && return err!(b, 401, "missing bearer token")
    st == AUTH_PAYLOAD && return err!(b, 401, "invalid token payload")
    err!(b, 401, "invalid or expired token")
end

function h_health!(b::Buf)
    if health_ok()
        putstr!(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
        putint!(b, floor(Int, time() - START[]))
        pushb!(b, UInt8('}'))
        return 200
    end
    msg = db_errmsg()
    putstr!(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
    GC.@preserve msg putjsonstr!(b, pointer(msg), sizeof(msg))
    pushb!(b, UInt8('}'))
    503
end

h_feed!(b::Buf) = feed!(READ, b) ? 200 : err!(b, 500, "internal server error")

function h_post!(b::Buf, id::Int)
    r = post!(READ, b, id)
    r == 1 ? 200 : r == 0 ? err!(b, 404, "post not found") : err!(b, 500, "internal server error")
end

# One code point at TXT[i]: (cp, nbytes); invalid UTF-8 gives (-1, 1).
@inline function utf8_at(d::Vector{UInt8}, i::Int, e::Int)
    @inbounds c = d[i]
    c < 0x80 && return (Int(c), 1)
    n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0
    (n == 0 || i + n - 1 > e) && return (-1, 1)
    cp = Int(c & (0x7f >> n))
    @inbounds for k in 1:n-1
        cp = (cp << 6) | Int(d[i+k] & 0x3f)
    end
    (cp, n)
end

# String.prototype.trim whitespace and line terminators.
@inline isjsspace(cp::Int) =
    (0x09 <= cp <= 0x0d) || cp == 0x20 || cp == 0xa0 || cp == 0x1680 || (0x2000 <= cp <= 0x200a) ||
    cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff

function h_create!(b::Buf, d::Vector{UInt8}, r::Req)
    st, uid = authenticate(d, r)
    st == AUTH_OK || return auth_error!(b, st)
    scan_doc!(BF, BKEY, d, r.bs, r.bs + r.bl - 1, ("body",)) || return err!(b, 400, "malformed JSON body")
    (BF.isobject && BF.typ[1] == JT_STRING) || return err!(b, 400, "body is required")
    decode_string!(TXT, d, BF.vs[1], BF.ve[1])
    t = TXT.data
    s = 1; e = TXT.len
    while s <= e
        cp, n = utf8_at(t, s, e)
        isjsspace(cp) || break
        s += n
    end
    while e >= s
        j = e
        @inbounds while j > s && (t[j] & 0xc0) == 0x80
            j -= 1
        end
        cp, n = utf8_at(t, j, e)
        (j + n - 1 == e && isjsspace(cp)) || break
        e = j - 1
    end
    e >= s || return err!(b, 400, "body is required")
    units = 0                                     # UTF-16 length, like JavaScript's .length
    @inbounds for i in s:e
        c = t[i]
        (c & 0xc0) == 0x80 && continue
        units += c >= 0xf0 ? 2 : 1
    end
    units <= 500 || return err!(b, 400, "body must be at most 500 characters")
    ok = GC.@preserve t create_post!(b, uid, pointer(t, s), e - s + 1, USERNAME)
    ok ? 201 : err!(b, 500, "internal server error")
end

function h_like!(b::Buf, d::Vector{UInt8}, r::Req, s::Int, e::Int)
    st, uid = authenticate(d, r)
    st == AUTH_OK || return auth_error!(b, st)
    id = parse_id(d, s, e)
    id > 0 || return err!(b, 400, "invalid post id")
    ex = post_exists(READ, id)
    ex == 0 && return err!(b, 404, "post not found")
    ex < 0 && return err!(b, 500, "internal server error")
    lk = insert_like(uid, id)
    lk < 0 && return err!(b, 500, "internal server error")
    putstr!(b, lk == 1 ? "{\"liked\":true,\"already_liked\":false,\"post_id\":" :
                         "{\"liked\":true,\"already_liked\":true,\"post_id\":")
    putint!(b, id)
    pushb!(b, UInt8('}'))
    lk == 1 ? 201 : 200
end

function route!(b::Buf, d::Vector{UInt8}, r::Req)
    ps = r.ps; pe = r.pe
    if r.method == M_GET
        bytes_eq(d, ps, pe, "/feed") && return h_feed!(b)
        bytes_eq(d, ps, pe, "/health") && return h_health!(b)
        if bytes_startswith(d, ps, pe, "/posts/") && !has_slash(d, ps + 7, pe)
            id = parse_id(d, ps + 7, pe)
            id > 0 || return err!(b, 400, "invalid post id")
            return h_post!(b, id)
        end
    elseif r.method == M_POST
        bytes_eq(d, ps, pe, "/posts") && return h_create!(b, d, r)
        if bytes_startswith(d, ps, pe, "/posts/") && pe - ps + 1 >= 12 &&
           bytes_eq(d, pe - 4, pe, "/like") && !has_slash(d, ps + 7, pe - 5)
            return h_like!(b, d, r, ps + 7, pe - 5)
        end
    end
    err!(b, 404, "not found")
end

const RESP = Buf(32768)

function handle!(out::Buf, d::Vector{UInt8}, r::Req)
    reset!(RESP)
    status = try
        route!(RESP, d, r)
    catch ex
        @error "handler failed" exception = (ex, catch_backtrace())
        err!(RESP, 500, "internal server error")
    end
    write_response!(out, status, RESP, r.keepalive)
end
