package feed.json;

/**
 * The whitespace set of JavaScript's {@code String.prototype.trim()} (WhiteSpace plus LineTerminator in ECMA-262),
 * applied to valid UTF-8 by code point.
 */
public final class JsTrim {
    private JsTrim() {}

    public static boolean isWhitespace(int cp) {
        if (cp <= 0x20) return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D);
        if (cp < 0xA0) return false;
        return cp == 0xA0
                || cp == 0x1680
                || (cp >= 0x2000 && cp <= 0x200A)
                || cp == 0x2028
                || cp == 0x2029
                || cp == 0x202F
                || cp == 0x205F
                || cp == 0x3000
                || cp == 0xFEFF;
    }

    private static boolean isAsciiWhitespace(int c) {
        return c == 0x20 || (c >= 0x09 && c <= 0x0D);
    }

    /** First index in {@code [from, to)} that is not trimmed whitespace; the range must be valid UTF-8. */
    public static int trimStart(byte[] b, int from, int to) {
        int p = from;
        while (p < to) {
            int c = b[p];
            if (c >= 0) {
                if (!isAsciiWhitespace(c)) return p;
                p++;
                continue;
            }
            int lead = c & 0xFF;
            int n = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (!isWhitespace(Utf8.decode(b, p))) return p;
            p += n;
        }
        return p;
    }

    /** End index (exclusive) after trimming whitespace at the end of {@code [from, to)}; the range must be valid UTF-8. */
    public static int trimEnd(byte[] b, int from, int to) {
        int e = to;
        while (e > from) {
            int c = b[e - 1];
            if (c >= 0) {
                if (!isAsciiWhitespace(c)) return e;
                e--;
                continue;
            }
            int s = e - 1;
            while (s > from && (b[s] & 0xC0) == 0x80) s--;
            if (!isWhitespace(Utf8.decode(b, s))) return e;
            e = s;
        }
        return e;
    }
}
