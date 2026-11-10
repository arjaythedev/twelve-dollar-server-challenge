/** HTTP/1.1 framing. Only what the API uses is read: the request line, Authorization,
 * Content-Length, Connection and Transfer-Encoding. The request parsed last is described by the
 * fields below, as absolute addresses into its bytes. */
final class Http {
    private Http() {}

    static final int MAX_HEAD = 16384;
    static final int MAX_BODY = 1048576;

    static boolean get, post, close, hasAuth;
    static long p0, p1;   // path, without the query
    static long a0, a1;   // Authorization value, trimmed
    static long b0, b1;   // body
    static long used;     // the request's length
    static long need;     // while it is incomplete: its length, once the head says (else 0)

    // Positions of blanks found by trim().
    private static long ts, te;

    private static final byte[] HTTP1 = Out.ascii("HTTP/1.");
    private static final byte[] HTTP10 = Out.ascii("HTTP/1.0");
    private static final byte[] GET = Out.ascii("GET");
    private static final byte[] POST = Out.ascii("POST");
    private static final byte[] AUTHORIZATION = Out.ascii("authorization");
    private static final byte[] CONTENT_LENGTH = Out.ascii("content-length");
    private static final byte[] TRANSFER_ENCODING = Out.ascii("transfer-encoding");
    private static final byte[] CONNECTION = Out.ascii("connection");
    private static final byte[] CLOSE = Out.ascii("close");
    private static final byte[] KEEP_ALIVE = Out.ascii("keep-alive");

    /** The first '\n' in a..end, or -1. Eight bytes at a time. */
    static long newline(long a, long end) {
        long p = a;
        while (p + 8 <= end) {
            long x = Out.load64(p) ^ 0x0a0a0a0a0a0a0a0aL;
            long m = (x - Out.ONES) & ~x & Out.HIGHS;
            if (m != 0) return p + (Long.numberOfTrailingZeros(m) >>> 3);
            p += 8;
        }
        for (; p < end; p++) if (N.u(p) == '\n') return p;
        return -1;
    }

    private static long position(long a, long end, int c) {
        for (long p = a; p < end; p++) if (N.u(p) == c) return p;
        return -1;
    }

    private static void trim(long s, long e) {
        while (s < e && (N.u(s) == ' ' || N.u(s) == '\t')) s++;
        while (e > s && (N.u(e - 1) == ' ' || N.u(e - 1) == '\t')) e--;
        ts = s;
        te = e;
    }

    /** ASCII case-insensitive comparison with a lower-case name. */
    static boolean isName(long a, long n, byte[] lower) {
        if (n != lower.length) return false;
        for (int k = 0; k < lower.length; k++) {
            int c = N.u(a + k);
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != lower[k]) return false;
        }
        return true;
    }

    /** Parses the request at the start of the n bytes at b: 1 a whole request (the fields set),
     * 0 more bytes are needed, -1 malformed. */
    static int parse(long b, long n) {
        need = 0;
        long end = b + n;
        long nl = newline(b, end);
        if (nl < 0) return n > MAX_HEAD ? -1 : 0;
        long le = nl;
        if (le > b && N.u(le - 1) == '\r') le--;
        long sp1 = position(b, le, ' ');
        if (sp1 <= b) return -1;
        long sp2 = position(sp1 + 1, le, ' ');
        if (sp2 <= sp1 + 1) return -1;
        long version = sp2 + 1;
        if (!Out.startsWith(version, le - version, HTTP1)) return -1;
        get = Out.eq(b, sp1 - b, GET);
        post = Out.eq(b, sp1 - b, POST);
        close = Out.eq(version, le - version, HTTP10);
        hasAuth = false;
        p0 = sp1 + 1;
        p1 = sp2;
        long q = position(p0, p1, '?');
        if (q >= 0) p1 = q;
        long clen = 0;
        long pos = nl + 1;
        for (;;) {
            long nx = newline(pos, end);
            if (nx < 0) return n > MAX_HEAD ? -1 : 0;
            long e = nx;
            long next = e + 1;
            if (e > pos && N.u(e - 1) == '\r') e--;
            if (e == pos) { pos = next; break; }
            long colon = position(pos, e, ':');
            if (colon > pos) {
                long len = colon - pos;
                if (len == 13 && isName(pos, len, AUTHORIZATION)) {
                    trim(colon + 1, e);
                    hasAuth = true;
                    a0 = ts;
                    a1 = te;
                } else if (len == 14 && isName(pos, len, CONTENT_LENGTH)) {
                    trim(colon + 1, e);
                    if (ts == te || te - ts > 9) return -1;
                    long v = 0;
                    for (long i = ts; i < te; i++) {
                        int c = N.u(i);
                        if (c < '0' || c > '9') return -1;
                        v = v * 10 + (c - '0');
                    }
                    if (v > MAX_BODY) return -1;
                    clen = v;
                } else if (len == 17 && isName(pos, len, TRANSFER_ENCODING)) {
                    return -1;  // chunked bodies are out of scope
                } else if (len == 10 && isName(pos, len, CONNECTION)) {
                    trim(colon + 1, e);
                    if (isName(ts, te - ts, CLOSE)) close = true;
                    else if (isName(ts, te - ts, KEEP_ALIVE)) close = false;
                }
            }
            pos = next;
            if (pos - b > MAX_HEAD) return -1;
        }
        if (n < pos - b + clen) { need = pos - b + clen; return 0; }
        b0 = pos;
        b1 = pos + clen;
        used = pos - b + clen;
        return 1;
    }
}
