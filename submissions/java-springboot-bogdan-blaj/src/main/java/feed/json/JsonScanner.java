package feed.json;

import java.util.Arrays;

/**
 * Strict RFC 8259 validator for one JSON document in a byte range: exactly one value with optional JSON whitespace
 * around it, raw bytes valid UTF-8, no raw control characters in strings, only the RFC escapes, {@code \\u} surrogate
 * escapes properly paired. Nesting deeper than {@link #MAX_DEPTH} is rejected. Iterative (no recursion) and
 * allocation-free after construction; single-threaded.
 *
 * <p>Members of the top-level object whose decoded key equals one of the configured keys are recorded in "slots"
 * (type and raw value span; the last occurrence wins, like {@code JSON.parse}). Slots are only meaningful after
 * {@link #scan} returned true.
 */
public final class JsonScanner {
    public static final int MAX_DEPTH = 512;

    public static final byte NONE = 0, OBJECT = 1, ARRAY = 2, STRING = 3, NUMBER = 4, TRUE = 5, FALSE = 6, NULL = 7;

    private static final int ST_VALUE = 0, ST_AFTER = 1, ST_KEY = 2;
    private static final byte[] LIT_TRUE = {'t', 'r', 'u', 'e'};
    private static final byte[] LIT_FALSE = {'f', 'a', 'l', 's', 'e'};
    private static final byte[] LIT_NULL = {'n', 'u', 'l', 'l'};

    private final byte[][] keys;
    private final byte[] slotType;
    private final int[] slotStart;
    private final int[] slotEnd;
    private final boolean[] slotEscaped;
    private final boolean[] inObject = new boolean[MAX_DEPTH];
    private byte[] keyScratch = new byte[64];
    private byte topType;
    /** Whether the string most recently passed to {@link #scanString} contained a backslash escape. */
    private boolean lastEscaped;

    public JsonScanner(byte[]... keys) {
        this.keys = keys.clone();
        int n = keys.length;
        slotType = new byte[n];
        slotStart = new int[n];
        slotEnd = new int[n];
        slotEscaped = new boolean[n];
    }

    /** Type of the top-level value after a successful scan. */
    public byte topType() {
        return topType;
    }

    /** Type of the value recorded for key {@code slot} of the top-level object, or {@link #NONE}. */
    public byte type(int slot) {
        return slotType[slot];
    }

    /** Start of the recorded value; for strings, the first byte after the opening quote. */
    public int start(int slot) {
        return slotStart[slot];
    }

    /** End (exclusive) of the recorded value; for strings, the index of the closing quote. */
    public int end(int slot) {
        return slotEnd[slot];
    }

    /** Whether the recorded string value contains backslash escapes (if not, its raw bytes are its UTF-8 value). */
    public boolean escaped(int slot) {
        return slotEscaped[slot];
    }

    /** Validates {@code b[off, off+len)} as one JSON document and records the configured top-level members. */
    public boolean scan(byte[] b, int off, int len) {
        Arrays.fill(slotType, NONE);
        topType = NONE;
        final int end = off + len;
        int p = skipWs(b, off, end);
        int depth = 0;
        int slot = -1;
        int state = ST_VALUE;
        while (true) {
            if (state == ST_VALUE) {
                if (p >= end) return false;
                int c = b[p];
                switch (c) {
                    case '{' -> {
                        mark(depth, slot, OBJECT, p, p, false);
                        if (depth == MAX_DEPTH) return false;
                        inObject[depth++] = true;
                        p = skipWs(b, p + 1, end);
                        if (p < end && b[p] == '}') {
                            p++;
                            depth--;
                            state = ST_AFTER;
                        } else {
                            state = ST_KEY;
                        }
                    }
                    case '[' -> {
                        mark(depth, slot, ARRAY, p, p, false);
                        if (depth == MAX_DEPTH) return false;
                        inObject[depth++] = false;
                        p = skipWs(b, p + 1, end);
                        if (p < end && b[p] == ']') {
                            p++;
                            depth--;
                            state = ST_AFTER;
                        }
                    }
                    case '"' -> {
                        int q = scanString(b, p + 1, end);
                        if (q < 0) return false;
                        mark(depth, slot, STRING, p + 1, q, lastEscaped);
                        p = q + 1;
                        state = ST_AFTER;
                    }
                    case 't' -> {
                        if (!literal(b, p, end, LIT_TRUE)) return false;
                        mark(depth, slot, TRUE, p, p + 4, false);
                        p += 4;
                        state = ST_AFTER;
                    }
                    case 'f' -> {
                        if (!literal(b, p, end, LIT_FALSE)) return false;
                        mark(depth, slot, FALSE, p, p + 5, false);
                        p += 5;
                        state = ST_AFTER;
                    }
                    case 'n' -> {
                        if (!literal(b, p, end, LIT_NULL)) return false;
                        mark(depth, slot, NULL, p, p + 4, false);
                        p += 4;
                        state = ST_AFTER;
                    }
                    default -> {
                        if (c != '-' && (c < '0' || c > '9')) return false;
                        int q = scanNumber(b, p, end);
                        if (q < 0) return false;
                        mark(depth, slot, NUMBER, p, q, false);
                        p = q;
                        state = ST_AFTER;
                    }
                }
                slot = -1;
            } else if (state == ST_AFTER) {
                p = skipWs(b, p, end);
                if (depth == 0) return p == end;
                if (p >= end) return false;
                int c = b[p];
                boolean obj = inObject[depth - 1];
                if (c == ',') {
                    p = skipWs(b, p + 1, end);
                    state = obj ? ST_KEY : ST_VALUE;
                } else if (c == (obj ? '}' : ']')) {
                    p++;
                    depth--;
                } else {
                    return false;
                }
            } else {
                if (p >= end || b[p] != '"') return false;
                int q = scanString(b, p + 1, end);
                if (q < 0) return false;
                slot = depth == 1 ? matchKey(b, p + 1, q, lastEscaped) : -1;
                p = skipWs(b, q + 1, end);
                if (p >= end || b[p] != ':') return false;
                p = skipWs(b, p + 1, end);
                state = ST_VALUE;
            }
        }
    }

    private void mark(int depth, int slot, byte type, int s, int e, boolean esc) {
        if (depth == 0) {
            topType = type;
        } else if (slot >= 0) {
            slotType[slot] = type;
            slotStart[slot] = s;
            slotEnd[slot] = e;
            slotEscaped[slot] = esc;
        }
    }

    private int matchKey(byte[] b, int s, int e, boolean esc) {
        byte[] k = b;
        int ks = s;
        int ke = e;
        if (esc) {
            if (keyScratch.length < e - s) keyScratch = new byte[Math.max(e - s, keyScratch.length * 2)];
            ke = decodeString(b, s, e, keyScratch, 0);
            k = keyScratch;
            ks = 0;
        }
        for (int i = 0; i < keys.length; i++) {
            byte[] key = keys[i];
            if (Arrays.equals(key, 0, key.length, k, ks, ke)) return i;
        }
        return -1;
    }

    private static int skipWs(byte[] b, int p, int end) {
        while (p < end) {
            byte c = b[p];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
            p++;
        }
        return p;
    }

    private static boolean literal(byte[] b, int p, int end, byte[] lit) {
        int n = lit.length;
        return end - p >= n && Arrays.equals(b, p, p + n, lit, 0, n);
    }

    private static boolean isDigit(byte c) {
        return c >= '0' && c <= '9';
    }

    /** Returns the end of the number starting at {@code p}, or -1. The caller checks what follows it. */
    private static int scanNumber(byte[] b, int p, int end) {
        if (b[p] == '-') p++;
        if (p >= end) return -1;
        if (b[p] == '0') {
            p++;
        } else if (isDigit(b[p])) {
            p++;
            while (p < end && isDigit(b[p])) p++;
        } else {
            return -1;
        }
        if (p < end && b[p] == '.') {
            p++;
            if (p >= end || !isDigit(b[p])) return -1;
            while (p < end && isDigit(b[p])) p++;
        }
        if (p < end && (b[p] == 'e' || b[p] == 'E')) {
            p++;
            if (p < end && (b[p] == '+' || b[p] == '-')) p++;
            if (p >= end || !isDigit(b[p])) return -1;
            while (p < end && isDigit(b[p])) p++;
        }
        return p;
    }

    private static int hex(byte c) {
        if (c >= '0' && c <= '9') return c - '0';
        int l = c | 0x20;
        if (l >= 'a' && l <= 'f') return l - 'a' + 10;
        return -1;
    }

    /** Four hex digits at {@code p} as a value 0..0xFFFF, or -1. */
    private static int hex4(byte[] b, int p, int end) {
        if (end - p < 4) return -1;
        return (hex(b[p]) << 12) | (hex(b[p + 1]) << 8) | (hex(b[p + 2]) << 4) | hex(b[p + 3]);
    }

    /**
     * Validates a string body starting after the opening quote; returns the index of the closing quote or -1.
     * Sets {@link #lastEscaped}.
     */
    private int scanString(byte[] b, int p, int end) {
        boolean esc = false;
        while (p < end) {
            byte c = b[p];
            if (c == '"') {
                lastEscaped = esc;
                return p;
            }
            if (c == '\\') {
                esc = true;
                if (p + 1 >= end) return -1;
                switch (b[p + 1]) {
                    case '"', '\\', '/', 'b', 'f', 'n', 'r', 't' -> p += 2;
                    case 'u' -> {
                        int u = hex4(b, p + 2, end);
                        if (u < 0) return -1;
                        p += 6;
                        if (u >= 0xD800 && u <= 0xDFFF) {
                            if (u >= 0xDC00) return -1;
                            if (end - p < 6 || b[p] != '\\' || b[p + 1] != 'u') return -1;
                            int lo = hex4(b, p + 2, end);
                            if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                            p += 6;
                        }
                    }
                    default -> {
                        return -1;
                    }
                }
            } else if (c >= 0x20) {
                p++;
            } else if (c < 0) {
                int n = Utf8.sequenceLength(b, p, end);
                if (n < 0) return -1;
                p += n;
            } else {
                return -1;
            }
        }
        return -1;
    }

    /**
     * Decodes the content of a string that {@link #scan} validated ({@code [start, end)} between the quotes) into
     * UTF-8 at {@code dst[dOff]}; returns the number of bytes written, which is at most {@code end - start}.
     */
    public static int decodeString(byte[] b, int start, int end, byte[] dst, int dOff) {
        int d = dOff;
        int p = start;
        while (p < end) {
            byte c = b[p];
            if (c != '\\') {
                dst[d++] = c;
                p++;
                continue;
            }
            byte e = b[p + 1];
            switch (e) {
                case 'b' -> dst[d++] = '\b';
                case 'f' -> dst[d++] = '\f';
                case 'n' -> dst[d++] = '\n';
                case 'r' -> dst[d++] = '\r';
                case 't' -> dst[d++] = '\t';
                case 'u' -> {
                    int cp = hex4(b, p + 2, end);
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        int lo = hex4(b, p + 8, end);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    d = Utf8.encode(cp, dst, d);
                    p += 4;
                }
                default -> dst[d++] = e;
            }
            p += 2;
        }
        return d - dOff;
    }

    /**
     * Exact {@code x > v} for a JSON number {@code x} spanning {@code b[s, e)} (already validated), without floating
     * point. Magnitudes beyond {@code Long.MAX_VALUE} saturate.
     */
    public static boolean numberGreaterThan(byte[] b, int s, int e, long v) {
        int p = s;
        boolean neg = false;
        if (b[p] == '-') {
            neg = true;
            p++;
        }
        int intStart = p;
        while (p < e && isDigit(b[p])) p++;
        int intEnd = p;
        int fracStart = p;
        int fracEnd = p;
        if (p < e && b[p] == '.') {
            p++;
            fracStart = p;
            while (p < e && isDigit(b[p])) p++;
            fracEnd = p;
        }
        long exp = 0;
        if (p < e) {
            p++;
            boolean expNeg = false;
            if (b[p] == '-') {
                expNeg = true;
                p++;
            } else if (b[p] == '+') {
                p++;
            }
            for (; p < e; p++) {
                if (exp < 1_000_000_000L) exp = exp * 10 + (b[p] - '0');
            }
            if (expNeg) exp = -exp;
        }
        int nInt = intEnd - intStart;
        int n = nInt + (fracEnd - fracStart);
        // x = D * 10^(exp - nFrac), where D is all digits; the first `keep` digits of D form the integer part.
        long keep = (long) nInt + exp;
        long ip = 0;
        boolean frac = false;
        for (int i = 0; i < n; i++) {
            int d = (i < nInt ? b[intStart + i] : b[fracStart + i - nInt]) - '0';
            if (i < keep) {
                ip = mul10Add(ip, d);
            } else if (d != 0) {
                frac = true;
                break;
            }
        }
        if (ip != 0) {
            for (long i = n; i < keep && ip != Long.MAX_VALUE; i++) ip = mul10Add(ip, 0);
        }
        if (!neg) return ip > v || (ip == v && frac);
        return v < 0 && ip < -v;
    }

    private static long mul10Add(long x, int d) {
        if (x > (Long.MAX_VALUE - d) / 10) return Long.MAX_VALUE;
        return x * 10 + d;
    }
}
