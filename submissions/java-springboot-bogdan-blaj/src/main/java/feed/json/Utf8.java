package feed.json;

/** UTF-8 helpers on byte ranges. Validation follows RFC 3629 (no overlongs, no surrogates, nothing above U+10FFFF). */
public final class Utf8 {
    private Utf8() {}

    /**
     * Length of the well-formed UTF-8 sequence that starts at {@code p}, or -1 if the bytes at {@code p} (bounded by
     * {@code end}) are not a well-formed sequence. Requires {@code p < end}.
     */
    public static int sequenceLength(byte[] b, int p, int end) {
        int c = b[p] & 0xFF;
        if (c < 0x80) return 1;
        if (c < 0xC2) return -1;
        if (c < 0xE0) {
            return p + 1 < end && isContinuation(b[p + 1]) ? 2 : -1;
        }
        if (c < 0xF0) {
            if (p + 2 >= end) return -1;
            int c1 = b[p + 1] & 0xFF;
            int lo = c == 0xE0 ? 0xA0 : 0x80;
            int hi = c == 0xED ? 0x9F : 0xBF;
            return c1 >= lo && c1 <= hi && isContinuation(b[p + 2]) ? 3 : -1;
        }
        if (c < 0xF5) {
            if (p + 3 >= end) return -1;
            int c1 = b[p + 1] & 0xFF;
            int lo = c == 0xF0 ? 0x90 : 0x80;
            int hi = c == 0xF4 ? 0x8F : 0xBF;
            return c1 >= lo && c1 <= hi && isContinuation(b[p + 2]) && isContinuation(b[p + 3]) ? 4 : -1;
        }
        return -1;
    }

    private static boolean isContinuation(byte x) {
        return (x & 0xC0) == 0x80;
    }

    /** Number of code points in a range that is already known to be valid UTF-8. */
    public static int codePointCount(byte[] b, int from, int to) {
        int n = 0;
        for (int p = from; p < to; p++) {
            if ((b[p] & 0xC0) != 0x80) n++;
        }
        return n;
    }

    /** Decodes the code point at {@code p} of a range that is already known to be valid UTF-8. */
    public static int decode(byte[] b, int p) {
        int c = b[p] & 0xFF;
        if (c < 0x80) return c;
        if (c < 0xE0) return ((c & 0x1F) << 6) | (b[p + 1] & 0x3F);
        if (c < 0xF0) return ((c & 0x0F) << 12) | ((b[p + 1] & 0x3F) << 6) | (b[p + 2] & 0x3F);
        return ((c & 0x07) << 18) | ((b[p + 1] & 0x3F) << 12) | ((b[p + 2] & 0x3F) << 6) | (b[p + 3] & 0x3F);
    }

    /** Encodes a scalar value (not a surrogate) at {@code pos}; returns the position after it. */
    public static int encode(int cp, byte[] dst, int pos) {
        if (cp < 0x80) {
            dst[pos] = (byte) cp;
            return pos + 1;
        }
        if (cp < 0x800) {
            dst[pos] = (byte) (0xC0 | (cp >> 6));
            dst[pos + 1] = (byte) (0x80 | (cp & 0x3F));
            return pos + 2;
        }
        if (cp < 0x10000) {
            dst[pos] = (byte) (0xE0 | (cp >> 12));
            dst[pos + 1] = (byte) (0x80 | ((cp >> 6) & 0x3F));
            dst[pos + 2] = (byte) (0x80 | (cp & 0x3F));
            return pos + 3;
        }
        dst[pos] = (byte) (0xF0 | (cp >> 18));
        dst[pos + 1] = (byte) (0x80 | ((cp >> 12) & 0x3F));
        dst[pos + 2] = (byte) (0x80 | ((cp >> 6) & 0x3F));
        dst[pos + 3] = (byte) (0x80 | (cp & 0x3F));
        return pos + 4;
    }
}
