# yyjson validates UTF-8 before Unicode trimming.

cdef int parse_positive_id(const char* p, size_t n, int64_t* value) noexcept nogil:
    cdef size_t i
    cdef uint64_t result = 0
    cdef int digit
    if n == 0:
        return -1
    for i in range(n):
        digit = <unsigned char>p[i] - 48
        if digit < 0 or digit > 9:
            return -1
        if result > (9223372036854775807ULL - digit) // 10:
            return -1
        result = result * 10 + digit
    if result == 0:
        return -1
    value[0] = result
    return 0

cdef yyjson_doc* read_json(Server* s, char* data, size_t n, size_t capacity) noexcept nogil:
    cdef uint32_t flags = 0
    # Borrow padded storage in situ; a full body uses yyjson's copying path.
    if data != NULL and capacity >= n and capacity-n >= YYJSON_PADDING_SIZE:
        memset(data+n,0,YYJSON_PADDING_SIZE)
        flags = YYJSON_READ_INSITU
    return yyjson_read_opts(data,n,flags,&s.json_allocator,NULL)

cdef uint32_t codepoint(const char* p, size_t* used) noexcept nogil:
    cdef uint32_t ch = <unsigned char>p[0]
    if ch < 128:
        used[0] = 1
        return ch
    if ch < 224:
        used[0] = 2
        return ((ch & 31) << 6) | (<unsigned char>p[1] & 63)
    if ch < 240:
        used[0] = 3
        return ((ch & 15) << 12) | ((<unsigned char>p[1] & 63) << 6) | (<unsigned char>p[2] & 63)
    used[0] = 4
    return ((ch & 7) << 18) | ((<unsigned char>p[1] & 63) << 12) | ((<unsigned char>p[2] & 63) << 6) | (<unsigned char>p[3] & 63)

cdef int whitespace(uint32_t ch) noexcept nogil:
    return ch == 32 or (ch >= 9 and ch <= 13) or ch == 160 or ch == 5760 or (ch >= 8192 and ch <= 8202) or ch == 8232 or ch == 8233 or ch == 8239 or ch == 8287 or ch == 12288 or ch == 65279

cdef int trim_body(const char* p, size_t n, size_t* start, size_t* end) noexcept nogil:
    cdef size_t i = 0, used, last = 0
    cdef int chars = 0, trailing = 0
    cdef uint32_t ch
    start[0] = n
    while i < n:
        ch = codepoint(p + i, &used)
        if not whitespace(ch):
            if start[0] == n: start[0] = i
            last = i + used
            trailing = 0
        else:
            trailing += 1
        if start[0] != n: chars += 1
        i += used
    end[0] = last
    return chars - trailing if start[0] != n else 0
