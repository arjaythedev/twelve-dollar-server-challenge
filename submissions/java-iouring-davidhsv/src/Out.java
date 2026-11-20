/** Bytes out: decimal numbers and the JSON writer. */
final class Out {
    private Out() {}

    static final long ONES = 0x0101010101010101L;
    static final long HIGHS = 0x8080808080808080L;
    private static final byte[] HEX = "0123456789abcdef".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

    /** Writes v in decimal at a and returns the number of bytes. */
    static int digits(long a, long v) {
        if (v < 0) {
            N.put(a, '-');
            if (v == Long.MIN_VALUE) {
                byte[] min = "9223372036854775808".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
                N.copy(min, 0, a + 1, min.length);
                return 1 + min.length;
            }
            return 1 + digits(a + 1, -v);
        }
        int n = width(v);
        long at = a + n;
        do {
            at--;
            N.put(at, (int) ('0' + v % 10));
            v /= 10;
        } while (v != 0);
        return n;
    }

    /** How many decimal digits a non-negative v has. */
    static int width(long v) {
        int n = 1;
        while (v >= 10) { v /= 10; n++; }
        return n;
    }

    static boolean eq(long a, long n, byte[] s) {
        if (n != s.length) return false;
        for (int k = 0; k < s.length; k++) if (N.b(a + k) != s[k]) return false;
        return true;
    }

    static boolean startsWith(long a, long n, byte[] s) {
        if (n < s.length) return false;
        for (int k = 0; k < s.length; k++) if (N.b(a + k) != s[k]) return false;
        return true;
    }

    // The first eight bytes at a (memory order), as a little- or big-endian word: special8 looks
    // at each byte on its own, so the order does not matter.
    static long load64(long a) { return N.M.get(N.LONG_U, a); }

    /** Nonzero when one of the eight bytes of x is a control character, a quote or a backslash:
     * the bytes JSON escapes inside a string. */
    static long special8(long x) {
        long quote = x ^ (ONES * 0x22);
        long slash = x ^ (ONES * 0x5c);
        return (((x - ONES * 0x20) & ~x) | ((quote - ONES) & ~quote) | ((slash - ONES) & ~slash)) & HIGHS;
    }

    /** True when none of the n bytes at a needs escaping. Eight bytes at a time; the last word
     * overlaps the one before it instead of being cut short. */
    static boolean isPlain(long a, long n) {
        if (n >= 8) {
            long found = 0;
            long p = a, end = a + n - 8;
            while (p < end) {
                found |= special8(load64(p));
                p += 8;
            }
            return (found | special8(load64(end))) == 0;
        }
        for (long p = a; p < a + n; p++) {
            int c = N.u(p);
            if (c < 0x20 || c == '"' || c == '\\') return false;
        }
        return true;
    }

    /** Appends the n bytes at a as the inside of a JSON string. */
    static void text(Buf out, long a, long n) {
        if (isPlain(a, n)) { out.put(a, n); return; }
        out.ensure(n * 6);
        long w = out.base + out.len;
        for (long p = a; p < a + n; p++) {
            int c = N.u(p);
            if (c == '"') { N.put(w++, '\\'); N.put(w++, '"'); }
            else if (c == '\\') { N.put(w++, '\\'); N.put(w++, '\\'); }
            else if (c >= 0x20) N.put(w++, c);
            else if (c == '\n') { N.put(w++, '\\'); N.put(w++, 'n'); }
            else if (c == '\r') { N.put(w++, '\\'); N.put(w++, 'r'); }
            else if (c == '\t') { N.put(w++, '\\'); N.put(w++, 't'); }
            else if (c == 8) { N.put(w++, '\\'); N.put(w++, 'b'); }
            else if (c == 12) { N.put(w++, '\\'); N.put(w++, 'f'); }
            else {
                N.put(w++, '\\'); N.put(w++, 'u'); N.put(w++, '0'); N.put(w++, '0');
                N.put(w++, HEX[c >> 4]);
                N.put(w++, HEX[c & 15]);
            }
        }
        out.len = w - out.base;
    }

    private static final byte[] P_CREATED = ascii("\",\"created_at\":\"");

    // The same constants as words in memory order (the machine is little-endian), written with
    // whole 8-byte stores: the bytes a store writes past a constant are overwritten by what
    // follows it, and post() has reserved room for them.
    private static long word(String s, int at) {
        long v = 0;
        for (int i = 0; i < 8 && at + i < s.length(); i++) v |= (long) (s.charAt(at + i) & 0xff) << (8 * i);
        return v;
    }
    private static final long ID0 = word("{\"id\":", 0);
    private static final long BODY0 = word(",\"body\":\"", 0), BODY1 = word(",\"body\":\"", 8);
    private static final long CREATED0 = word("\",\"created_at\":\"", 0), CREATED1 = word("\",\"created_at\":\"", 8);
    private static final long AUTHOR0 = word("\",\"author\":\"", 0), AUTHOR1 = word("\",\"author\":\"", 8);
    private static final long LIKES0 = word("\",\"like_count\":", 0), LIKES1 = word("\",\"like_count\":", 8);
    static {
        if (java.nio.ByteOrder.nativeOrder() != java.nio.ByteOrder.LITTLE_ENDIAN) throw new IllegalStateException("big-endian");
    }
    // Strings of a vector's width and more are copied and checked a vector at a time (1-3 % less
    // CPU per request than eight bytes at a time, measured on x86-64); -Dfeed.scalar=true (a test
    // knob) keeps to eight bytes.
    private static final int VECTOR_MIN = Boolean.getBoolean("feed.scalar") ? Integer.MAX_VALUE : Vec.width();

    /** Copies the n bytes at from to to, eight at a time, and says whether any of them needs
     * escaping in a JSON string: nonzero if one does. One pass reads each byte once for both. */
    private static long copyCheck(long from, long to, long n) {
        if (n >= VECTOR_MIN) return Vec.copyCheck(from, to, n);
        if (n >= 8) {
            long found = 0;
            long last = n - 8;
            for (long i = 0; i < last; i += 8) {
                long x = load64(from + i);
                N.M.set(N.LONG_U, to + i, x);
                found |= special8(x);
            }
            long x = load64(from + last);
            N.M.set(N.LONG_U, to + last, x);
            return found | special8(x);
        }
        long found = 0;
        for (long i = 0; i < n; i++) {
            int c = N.u(from + i);
            N.put(to + i, c);
            if (c < 0x20 || c == '"' || c == '\\') found = 1;
        }
        return found;
    }

    /** One post object. */
    static void post(Buf out, long id, long body, long bodyLen, long created, long createdLen, long author,
                     long authorLen, long likes) {
        // the constants (59 bytes), two numbers and the three strings unescaped, and a word of slack
        out.ensure(59 + 40 + bodyLen + createdLen + authorLen + 8);
        long p = out.base + out.len;
        N.M.set(N.LONG_U, p, ID0);
        p += 6;
        p += digits(p, id);
        N.M.set(N.LONG_U, p, BODY0);
        N.M.set(N.LONG_U, p + 8, BODY1);
        p += 9;
        long mark = p;
        long bad = copyCheck(body, p, bodyLen);
        p += bodyLen;
        N.M.set(N.LONG_U, p, CREATED0);
        N.M.set(N.LONG_U, p + 8, CREATED1);
        p += 16;
        bad |= copyCheck(created, p, createdLen);
        p += createdLen;
        if (bad != 0) {
            out.len = mark - out.base;
            text(out, body, bodyLen);
            out.put(P_CREATED);
            text(out, created, createdLen);
            out.ensure(12 + authorLen + 15 + 20 + 1 + 8);
            p = out.base + out.len;
        }
        N.M.set(N.LONG_U, p, AUTHOR0);
        N.M.set(N.LONG_U, p + 8, AUTHOR1);
        p += 12;
        mark = p;
        if (copyCheck(author, p, authorLen) != 0) {
            out.len = mark - out.base;
            text(out, author, authorLen);
            out.ensure(15 + 20 + 1 + 8);
            p = out.base + out.len;
        } else {
            p += authorLen;
        }
        N.M.set(N.LONG_U, p, LIKES0);
        N.M.set(N.LONG_U, p + 8, LIKES1);
        p += 15;
        p += digits(p, likes);
        N.put(p++, '}');
        out.len = p - out.base;
    }

    static byte[] ascii(String s) { return s.getBytes(java.nio.charset.StandardCharsets.UTF_8); }
}
