package app;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

/**
 * A strict RFC 8259 scanner for the JWT header and payload: recursive descent straight over native
 * memory, no tree, no String, no allocation. It validates the whole document (anything it does not
 * care about is still checked, then skipped) and materialises only the four top-level members that
 * matter: alg, sub, username, exp. Duplicate keys: the last one wins, like JSON.parse.
 *
 * One instance per Ctx, so it is never shared between threads.
 */
final class JsonScan {
    static final int ERR = -1, STRING = 1, NUMBER = 2, OBJECT = 3, ARRAY = 4, TRUE = 5, FALSE = 6, NUL = 7;
    private static final int MAX_DEPTH = 32;

    private static final byte[] K_ALG = {'a', 'l', 'g'};
    private static final byte[] K_SUB = {'s', 'u', 'b'};
    private static final byte[] K_EXP = {'e', 'x', 'p'};
    private static final byte[] K_USERNAME = {'u', 's', 'e', 'r', 'n', 'a', 'm', 'e'};
    private static final byte[] L_TRUE = {'t', 'r', 'u', 'e'};
    private static final byte[] L_FALSE = {'f', 'a', 'l', 's', 'e'};
    private static final byte[] L_NULL = {'n', 'u', 'l', 'l'};

    private MemorySegment s;
    private long p, end;
    private int depth;

    // span of the most recently parsed string / value of the most recently parsed number
    private long strOff;
    private int strLen;
    private boolean strEsc;
    private long numVal;

    boolean algString;
    long algOff;
    int algLen;
    boolean algEsc;

    boolean subFound, subString;
    long subOff;
    int subLen;
    boolean subEsc;

    boolean nameFound, nameString;
    long nameOff;
    int nameLen;
    boolean nameEsc;

    boolean expFound, expNumber;
    long expVal;

    /** True if [off, off+len) is exactly one valid JSON object. */
    boolean parseObject(MemorySegment seg, long off, int len) {
        s = seg;
        p = off;
        end = off + len;
        depth = 0;
        algString = false; algLen = 0; algEsc = false;
        subFound = false; subString = false; subLen = 0; subEsc = false;
        nameFound = false; nameString = false; nameLen = 0; nameEsc = false;
        expFound = false; expNumber = false; expVal = 0;
        skipWs();
        if (p >= end || b(p) != '{') return false;
        if (!object(true)) return false;
        skipWs();
        return p == end;
    }

    private byte b(long i) { return s.get(ValueLayout.JAVA_BYTE, i); }

    private void skipWs() {
        while (p < end) {
            byte c = b(p);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p++;
            else break;
        }
    }

    private boolean object(boolean top) {
        if (++depth > MAX_DEPTH) return false;
        p++;
        skipWs();
        if (p < end && b(p) == '}') { p++; depth--; return true; }
        for (;;) {
            skipWs();
            if (p >= end || b(p) != '"') return false;
            if (!string()) return false;
            long kOff = strOff;
            int kLen = strLen;
            boolean kEsc = strEsc;
            skipWs();
            if (p >= end || b(p) != ':') return false;
            p++;
            skipWs();
            int kind = value();
            if (kind == ERR) return false;
            if (top && !kEsc) capture(kOff, kLen, kind);
            skipWs();
            if (p >= end) return false;
            byte c = b(p);
            if (c == ',') { p++; continue; }
            if (c == '}') { p++; depth--; return true; }
            return false;
        }
    }

    private boolean array() {
        if (++depth > MAX_DEPTH) return false;
        p++;
        skipWs();
        if (p < end && b(p) == ']') { p++; depth--; return true; }
        for (;;) {
            skipWs();
            if (value() == ERR) return false;
            skipWs();
            if (p >= end) return false;
            byte c = b(p);
            if (c == ',') { p++; continue; }
            if (c == ']') { p++; depth--; return true; }
            return false;
        }
    }

    private int value() {
        if (p >= end) return ERR;
        switch (b(p)) {
            case '"': return string() ? STRING : ERR;
            case '{': return object(false) ? OBJECT : ERR;
            case '[': return array() ? ARRAY : ERR;
            case 't': return literal(L_TRUE) ? TRUE : ERR;
            case 'f': return literal(L_FALSE) ? FALSE : ERR;
            case 'n': return literal(L_NULL) ? NUL : ERR;
            default: return number() ? NUMBER : ERR;
        }
    }

    private boolean literal(byte[] lit) {
        if (p + lit.length > end) return false;
        for (int i = 0; i < lit.length; i++) if (b(p + i) != lit[i]) return false;
        p += lit.length;
        return true;
    }

    /** On entry p is at '"'. Captures the raw (still escaped) content span. */
    private boolean string() {
        p++;
        long st = p;
        boolean esc = false;
        for (;;) {
            if (p >= end) return false;
            int c = b(p) & 0xFF;
            if (c == '"') break;
            if (c == '\\') {
                esc = true;
                p++;
                if (p >= end) return false;
                int e = b(p) & 0xFF;
                if (e == 'u') {
                    if (p + 4 >= end) return false;
                    for (int i = 1; i <= 4; i++) if (hex(b(p + i)) < 0) return false;
                    p += 5;
                } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f'
                        || e == 'n' || e == 'r' || e == 't') {
                    p++;
                } else {
                    return false;
                }
            } else if (c < 0x20) {
                return false;
            } else {
                p++;
            }
        }
        strOff = st;
        strLen = (int) (p - st);
        strEsc = esc;
        p++;
        return true;
    }

    /**
     * Strict JSON number. Keeps the integer value (truncated towards zero), which is all an "exp"
     * claim ever needs; a value too large to hold saturates, so it stays on the same side of now.
     */
    private boolean number() {
        boolean neg = false;
        if (b(p) == '-') { p++; neg = true; }
        if (p >= end) return false;
        int c = b(p) & 0xFF;
        long v = 0;
        boolean sat = false;
        if (c == '0') {
            p++;
        } else if (c >= '1' && c <= '9') {
            while (p < end && digit(b(p))) {
                int d = b(p) - '0';
                if (v > (Long.MAX_VALUE - d) / 10) sat = true; else v = v * 10 + d;
                p++;
            }
        } else {
            return false;
        }
        long fracOff = 0;
        int fracLen = 0;
        if (p < end && b(p) == '.') {
            p++;
            if (p >= end || !digit(b(p))) return false;
            fracOff = p;
            while (p < end && digit(b(p))) p++;
            fracLen = (int) (p - fracOff);
        }
        int exp = 0;
        if (p < end && (b(p) == 'e' || b(p) == 'E')) {
            p++;
            boolean eneg = false;
            if (p < end && (b(p) == '+' || b(p) == '-')) { eneg = b(p) == '-'; p++; }
            if (p >= end || !digit(b(p))) return false;
            int e = 0;
            while (p < end && digit(b(p))) { if (e < 1000) e = e * 10 + (b(p) - '0'); p++; }
            exp = eneg ? -e : e;
        }
        if (sat) {
            numVal = neg ? Long.MIN_VALUE : Long.MAX_VALUE;
            return true;
        }
        long r = v;
        int e = exp;
        int fi = 0;
        while (e > 0 && fi < fracLen) {              // shift fraction digits into the integer part
            int d = b(fracOff + fi) - '0';
            if (r > (Long.MAX_VALUE - d) / 10) { r = Long.MAX_VALUE; e = 0; break; }
            r = r * 10 + d;
            fi++;
            e--;
        }
        while (e > 0) {
            if (r > Long.MAX_VALUE / 10) { r = Long.MAX_VALUE; break; }
            r *= 10;
            e--;
        }
        while (e < 0 && r != 0) { r /= 10; e++; }
        numVal = neg ? -r : r;
        return true;
    }

    private void capture(long kOff, int kLen, int kind) {
        if (kLen == 3) {
            if (eq(kOff, K_ALG)) {
                algString = kind == STRING;
                if (algString) { algOff = strOff; algLen = strLen; algEsc = strEsc; }
            } else if (eq(kOff, K_SUB)) {
                subFound = true;
                subString = kind == STRING;
                if (subString) { subOff = strOff; subLen = strLen; subEsc = strEsc; }
            } else if (eq(kOff, K_EXP)) {
                expFound = true;
                expNumber = kind == NUMBER;
                if (expNumber) expVal = numVal;
            }
        } else if (kLen == 8 && eq(kOff, K_USERNAME)) {
            nameFound = true;
            nameString = kind == STRING;
            if (nameString) { nameOff = strOff; nameLen = strLen; nameEsc = strEsc; }
        }
    }

    private boolean eq(long off, byte[] key) {
        for (int i = 0; i < key.length; i++) if (b(off + i) != key[i]) return false;
        return true;
    }

    private static boolean digit(byte c) { return c >= '0' && c <= '9'; }

    private static int hex(byte c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    /**
     * Decodes an escaped JSON string span into dst as UTF-8. Only used when a token actually
     * contains an escape, which the load test never does. Returns the byte length, or -1.
     */
    static int decode(MemorySegment s, long off, int len, MemorySegment dst, long dstOff, int dstCap) {
        int o = 0;
        long i = off, e = off + len;
        while (i < e) {
            int c = s.get(ValueLayout.JAVA_BYTE, i) & 0xFF;
            if (c != '\\') {
                if (o >= dstCap) return -1;
                dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) c);
                o++;
                i++;
                continue;
            }
            i++;
            if (i >= e) return -1;
            int x = s.get(ValueLayout.JAVA_BYTE, i) & 0xFF;
            i++;
            int cp;
            switch (x) {
                case '"': cp = '"'; break;
                case '\\': cp = '\\'; break;
                case '/': cp = '/'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u': {
                    if (i + 4 > e) return -1;
                    cp = hex4(s, i);
                    if (cp < 0) return -1;
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= e
                            && s.get(ValueLayout.JAVA_BYTE, i) == '\\'
                            && s.get(ValueLayout.JAVA_BYTE, i + 1) == 'u') {
                        int lo = hex4(s, i + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   // unpaired surrogate
                    break;
                }
                default: return -1;
            }
            int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (o + n > dstCap) return -1;
            switch (n) {
                case 1:
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) cp);
                    break;
                case 2:
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (0xC0 | (cp >> 6)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 1, (byte) (0x80 | (cp & 0x3F)));
                    break;
                case 3:
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (0xE0 | (cp >> 12)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 1, (byte) (0x80 | ((cp >> 6) & 0x3F)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 2, (byte) (0x80 | (cp & 0x3F)));
                    break;
                default:
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o, (byte) (0xF0 | (cp >> 18)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 1, (byte) (0x80 | ((cp >> 12) & 0x3F)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 2, (byte) (0x80 | ((cp >> 6) & 0x3F)));
                    dst.set(ValueLayout.JAVA_BYTE, dstOff + o + 3, (byte) (0x80 | (cp & 0x3F)));
                    break;
            }
            o += n;
        }
        return o;
    }

    private static int hex4(MemorySegment s, long i) {
        int v = 0;
        for (int k = 0; k < 4; k++) {
            int d = hex(s.get(ValueLayout.JAVA_BYTE, i + k));
            if (d < 0) return -1;
            v = (v << 4) | d;
        }
        return v;
    }
}
