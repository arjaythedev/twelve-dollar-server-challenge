# JWT documents borrow reactor scratch storage through handler completion.

cdef struct Auth:
    int64_t user_id
    const char* username
    size_t username_len
    yyjson_doc* doc

cdef int decode_base64url(const char* p, size_t n, char* out, size_t cap) noexcept nogil:
    cdef size_t i, used = 0
    cdef uint32_t bits = 0
    cdef int count = 0, v
    cdef unsigned char ch
    if n % 4 == 1:
        return -1
    for i in range(n):
        ch = <unsigned char>p[i]
        if ch >= 65 and ch <= 90: v = ch - 65
        elif ch >= 97 and ch <= 122: v = ch - 97 + 26
        elif ch >= 48 and ch <= 57: v = ch - 48 + 52
        elif ch == 45: v = 62
        elif ch == 95: v = 63
        else: return -1
        bits = (bits << 6) | v
        count += 6
        if count >= 8:
            count -= 8
            if used >= cap:
                return -1
            out[used] = (bits >> count) & 255
            used += 1
    if count and bits & ((1 << count) - 1):
        return -1
    return used

cdef int authenticate(Server* s, Request* r, Auth* auth, const char** error) noexcept nogil:
    cdef const char* p
    cdef size_t n, i, first = 0, second = 0
    cdef int hlen, plen, siglen
    cdef yyjson_doc* head
    cdef yyjson_val* root
    cdef yyjson_val* val
    cdef double now = time(NULL)
    if r.auth.len < 7 or memcmp(r.auth.data, "Bearer ", 7) != 0:
        error[0] = "missing bearer token"
        return -1
    error[0] = "invalid or expired token"
    p = r.auth.data + 7
    n = r.auth.len - 7
    for i in range(n):
        if p[i] == 46:
            if not first: first = i
            elif not second: second = i
            else: return -1
    if first == 0 or second <= first + 1:
        return -1
    hlen = decode_base64url(p, first, s.decoded_header, sizeof(s.decoded_header))
    plen = decode_base64url(p + first + 1, second - first - 1, s.decoded_payload, sizeof(s.decoded_payload))
    siglen = decode_base64url(p + second + 1, n - second - 1, s.decoded_signature, sizeof(s.decoded_signature))
    if hlen <= 0 or plen <= 0 or siglen != 32:
        return -1
    head = read_json(s,s.decoded_header,hlen,sizeof(s.decoded_header))
    if head == NULL:
        return -1
    val = yyjson_obj_get(yyjson_doc_get_root(head), "alg")
    if not yyjson_is_str(val) or yyjson_get_len(val) != 5 or memcmp(yyjson_get_str(val), "HS256", 5) != 0:
        yyjson_doc_free(head)
        return -1
    yyjson_doc_free(head)
    if hmac_verify(s, p, second, s.decoded_signature) != 0:
        return -1
    auth.doc = read_json(s,s.decoded_payload,plen,sizeof(s.decoded_payload))
    if auth.doc == NULL:
        return -1
    root = yyjson_doc_get_root(auth.doc)
    if not yyjson_is_obj(root):
        return -1
    val = yyjson_obj_get(root, "exp")
    if val != NULL:
        if not yyjson_is_num(val) or now >= yyjson_get_num(val):
            return -1
    val = yyjson_obj_get(root, "nbf")
    if val != NULL:
        if not yyjson_is_num(val) or now < yyjson_get_num(val):
            return -1
    error[0] = "invalid token payload"
    val = yyjson_obj_get(root, "sub")
    if not yyjson_is_str(val) or parse_positive_id(yyjson_get_str(val), yyjson_get_len(val), &auth.user_id) != 0:
        return -1
    val = yyjson_obj_get(root, "username")
    if not yyjson_is_str(val):
        return -1
    auth.username = yyjson_get_str(val)
    auth.username_len = yyjson_get_len(val)
    return 0
