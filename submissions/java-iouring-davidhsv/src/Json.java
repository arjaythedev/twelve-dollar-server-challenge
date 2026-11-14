/** JSON: a validating reader over native memory. A document is checked whole (any nesting, with
 * an explicit stack, so no input can recurse), and the members of a top-level object that the
 * caller names are reported. Member names are compared decoded, and the last duplicate wins, as
 * in JSON.parse. Positions are absolute addresses; -1 means "not valid JSON". */
final class Json {
    private Json() {}

    static final int NONE = 0, STRING = 1, NUMBER = 2, OTHER = 3;

    /** The reported members: for a string, s..e is its inside (still escaped when esc); for a
     * number, its text. */
    static final int[] kind = new int[3];
    static final long[] s = new long[3];
    static final long[] e = new long[3];
    static final boolean[] esc = new boolean[3];

    /** Set by string(): whether the string it accepted has escapes. */
    private static boolean lastEsc;
    private static final Buf keybuf = new Buf(256);
    private static final int MAX_DEPTH = 512;
    private static final byte[] open = new byte[MAX_DEPTH]; // 1 an object, 2 an array

    // Bytes that end a run of plain string content: the quote, the backslash, control characters
    // and everything outside ASCII.
    private static final boolean[] STOPS = new boolean[256];
    static {
        for (int i = 0; i < 32; i++) STOPS[i] = true;
        for (int i = 128; i < 256; i++) STOPS[i] = true;
        STOPS['"'] = true;
        STOPS['\\'] = true;
    }

    static long ws(long i, long end) {
        while (i < end) {
            int c = N.u(i);
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
            i++;
        }
        return i;
    }

    static int hex(int c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    /** The length of the well-formed UTF-8 sequence at i (a lead byte >= 0x80), or -1. */
    static int utf8Len(long i, long end) {
        int c = N.u(i);
        if (c < 0xc2) return -1;
        if (c < 0xe0) {
            if (i + 1 >= end || (N.u(i + 1) & 0xc0) != 0x80) return -1;
            return 2;
        }
        if (c < 0xf0) {
            if (i + 2 >= end || (N.u(i + 1) & 0xc0) != 0x80 || (N.u(i + 2) & 0xc0) != 0x80) return -1;
            if (c == 0xe0 && N.u(i + 1) < 0xa0) return -1;   // overlong
            if (c == 0xed && N.u(i + 1) >= 0xa0) return -1;  // a surrogate
            return 3;
        }
        if (c > 0xf4 || i + 3 >= end) return -1;
        if ((N.u(i + 1) & 0xc0) != 0x80 || (N.u(i + 2) & 0xc0) != 0x80 || (N.u(i + 3) & 0xc0) != 0x80) return -1;
        if (c == 0xf0 && N.u(i + 1) < 0x90) return -1;       // overlong
        if (c == 0xf4 && N.u(i + 1) >= 0x90) return -1;      // past U+10FFFF
        return 4;
    }

    /** The first byte at or after i (before end) that STOPS a run of plain content, or -1. Eight
     * bytes at a time: the lowest byte a word flags is always a true one. */
    private static long stop(long i, long end) {
        while (i + 8 <= end) {
            long x = Out.load64(i);
            long m = Out.special8(x) | (x & Out.HIGHS);
            if (m != 0) return i + (Long.numberOfTrailingZeros(m) >>> 3);
            i += 8;
        }
        while (i < end) {
            if (STOPS[N.u(i)]) return i;
            i++;
        }
        return -1;
    }

    /** The string whose opening quote is at `at`: the position after its closing quote (-1 if it
     * is not a valid string); whether it has escapes is left in lastEsc. */
    static long string(long at, long end) {
        long i = at + 1;
        boolean escaped = false;
        for (;;) {
            i = stop(i, end);
            if (i < 0) return -1;
            int c = N.u(i);
            if (c == '"') { lastEsc = escaped; return i + 1; }
            if (c < 0x20) return -1;
            if (c == '\\') {
                escaped = true;
                if (i + 1 >= end) return -1;
                int d = N.u(i + 1);
                if (d == 'u') {
                    if (i + 5 >= end) return -1;
                    for (int h = 2; h < 6; h++) if (hex(N.u(i + h)) < 0) return -1;
                    i += 6;
                } else if (d == '"' || d == '\\' || d == '/' || d == 'b' || d == 'f' || d == 'n' || d == 'r' || d == 't') {
                    i += 2;
                } else {
                    return -1;
                }
            } else {
                int n = utf8Len(i, end);
                if (n < 0) return -1;
                i += n;
            }
        }
    }

    private static boolean digit(long i, long end) {
        if (i >= end) return false;
        int c = N.u(i);
        return c >= '0' && c <= '9';
    }

    /** The number at `at`: the position after it, or -1. */
    static long number(long at, long end) {
        long i = at;
        if (i < end && N.u(i) == '-') i++;
        if (i >= end) return -1;
        int c = N.u(i);
        if (c == '0') i++;
        else if (c >= '1' && c <= '9') { while (digit(i, end)) i++; }
        else return -1;
        if (i < end && N.u(i) == '.') {
            i++;
            if (!digit(i, end)) return -1;
            while (digit(i, end)) i++;
        }
        if (i < end && (N.u(i) == 'e' || N.u(i) == 'E')) {
            i++;
            if (i < end && (N.u(i) == '+' || N.u(i) == '-')) i++;
            if (!digit(i, end)) return -1;
            while (digit(i, end)) i++;
        }
        return i;
    }

    /** A member name at `at`, then a colon: the position after the colon, or -1. */
    private static long name(long at, long end) {
        if (at >= end || N.u(at) != '"') return -1;
        long e = string(at, end);
        if (e < 0) return -1;
        long i = ws(e, end);
        if (i >= end || N.u(i) != ':') return -1;
        return i + 1;
    }

    private static boolean word(long i, long end, String w) {
        if (end - i < w.length()) return false;
        for (int k = 0; k < w.length(); k++) if (N.u(i + k) != w.charAt(k)) return false;
        return true;
    }

    /** The value at `at` (after blanks): the position after it, or -1 if it is not valid JSON. */
    static long value(long at, long end) {
        int depth = 0;
        boolean want = true;
        long i = at;
        for (;;) {
            i = ws(i, end);
            if (i >= end) return -1;
            int c = N.u(i);
            if (want) {
                if (c == '{' || c == '[') {
                    if (depth == MAX_DEPTH) return -1;
                    open[depth] = (byte) (c == '{' ? 1 : 2);
                    depth++;
                    i = ws(i + 1, end);
                    if (i >= end) return -1;
                    if (N.u(i) == (c == '{' ? '}' : ']')) {
                        i++;
                        depth--;
                        want = false;
                    } else if (c == '{') {
                        i = name(i, end);
                        if (i < 0) return -1;
                    }
                } else {
                    if (c == '"') i = string(i, end);
                    else if (c == '-' || (c >= '0' && c <= '9')) i = number(i, end);
                    else if (word(i, end, "true") || word(i, end, "null")) i += 4;
                    else if (word(i, end, "false")) i += 5;
                    else return -1;
                    if (i < 0) return -1;
                    want = false;
                }
            } else {
                int top = open[depth - 1];
                if (c == ',') {
                    i++;
                    if (top == 1) {
                        i = name(ws(i, end), end);
                        if (i < 0) return -1;
                    }
                    want = true;
                } else if ((top == 1 && c == '}') || (top == 2 && c == ']')) {
                    i++;
                    depth--;
                } else {
                    return -1;
                }
            }
            if (!want && depth == 0) return i;
        }
    }

    private static boolean nameIs(long s, long e, boolean escaped, byte[] key) {
        if (!escaped) return Out.eq(s, e - s, key);
        keybuf.clear();
        unescape(s, e, keybuf);
        return keybuf.is(key);
    }

    /** Checks the document b..end and reports up to three members of its top-level object by
     * name (nkeys of them are looked for), in kind/s/e/esc[0..2]. 1 an object, 0 valid JSON of
     * another kind, -1 not JSON. */
    static int members(long b, long end, int nkeys, byte[] k0, byte[] k1, byte[] k2) {
        kind[0] = NONE;
        kind[1] = NONE;
        kind[2] = NONE;
        long i = ws(b, end);
        if (i >= end) return -1;
        if (N.u(i) != '{') {
            i = value(i, end);
            if (i < 0 || ws(i, end) != end) return -1;
            return 0;
        }
        i = ws(i + 1, end);
        if (i < end && N.u(i) == '}') return ws(i + 1, end) == end ? 1 : -1;
        for (;;) {
            if (i >= end || N.u(i) != '"') return -1;
            long ne = string(i, end);
            if (ne < 0) return -1;
            boolean nesc = lastEsc;
            long n0 = i + 1;
            long n1 = ne - 1;
            i = ws(ne, end);
            if (i >= end || N.u(i) != ':') return -1;
            long v0 = ws(i + 1, end);
            if (v0 >= end) return -1;
            int mk;
            long ms = v0, me;
            boolean mesc = false;
            int c = N.u(v0);
            if (c == '"') {
                long se = string(v0, end);
                if (se < 0) return -1;
                mk = STRING;
                ms = v0 + 1;
                me = se - 1;
                mesc = lastEsc;
                i = se;
            } else {
                i = value(v0, end);
                if (i < 0) return -1;
                me = i;
                mk = c == '-' || (c >= '0' && c <= '9') ? NUMBER : OTHER;
            }
            int slot = -1;
            if (nkeys > 0 && nameIs(n0, n1, nesc, k0)) slot = 0;
            else if (nkeys > 1 && nameIs(n0, n1, nesc, k1)) slot = 1;
            else if (nkeys > 2 && nameIs(n0, n1, nesc, k2)) slot = 2;
            if (slot >= 0) {
                kind[slot] = mk;
                s[slot] = ms;
                e[slot] = me;
                esc[slot] = mesc;
            }
            i = ws(i, end);
            if (i >= end) return -1;
            if (N.u(i) == '}') return ws(i + 1, end) == end ? 1 : -1;
            if (N.u(i) != ',') return -1;
            i = ws(i + 1, end);
        }
    }

    private static int hex4(long i) {
        return (hex(N.u(i)) << 12) | (hex(N.u(i + 1)) << 8) | (hex(N.u(i + 2)) << 4) | hex(N.u(i + 3));
    }

    /** Decodes the inside of a string that string() accepted, appending to out. A surrogate
     * escape without its pair becomes U+FFFD, as it does when JavaScript writes the string out as
     * UTF-8. */
    static void unescape(long s, long e, Buf out) {
        long i = s;
        while (i < e) {
            int c = N.u(i);
            if (c != '\\') { out.put(c); i++; continue; }
            int d = N.u(i + 1);
            i += 2;
            if (d == 'u') {
                int cp = hex4(i);
                i += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && i + 5 < e && N.u(i) == '\\' && N.u(i + 1) == 'u') {
                    int lo = hex4(i + 2);
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        i += 6;
                    }
                }
                if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd;
                utf8(out, cp);
            } else {
                out.put(switch (d) { case 'b' -> 8; case 'f' -> 12; case 'n' -> 10; case 'r' -> 13; case 't' -> 9; default -> d; });
            }
        }
    }

    static void utf8(Buf out, int c) {
        if (c < 0 || c > 0x10ffff) c = 0xfffd;
        if (c < 0x80) {
            out.put(c);
        } else if (c < 0x800) {
            out.put(0xc0 | (c >> 6));
            out.put(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out.put(0xe0 | (c >> 12));
            out.put(0x80 | ((c >> 6) & 0x3f));
            out.put(0x80 | (c & 0x3f));
        } else {
            out.put(0xf0 | (c >> 18));
            out.put(0x80 | ((c >> 12) & 0x3f));
            out.put(0x80 | ((c >> 6) & 0x3f));
            out.put(0x80 | (c & 0x3f));
        }
    }

    /** The decoded text of reported member k. */
    static void text(int k, Buf out) {
        if (esc[k]) unescape(s[k], e[k], out);
        else out.put(s[k], e[k] - s[k]);
    }
}
