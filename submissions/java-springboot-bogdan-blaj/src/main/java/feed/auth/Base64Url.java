package feed.auth;

import java.util.Arrays;

/** Base64url (RFC 4648 section 5) decoding on byte ranges. Padding is optional but, if present, must be correct. */
final class Base64Url {
    private Base64Url() {}

    private static final byte[] TABLE = new byte[256];

    static {
        Arrays.fill(TABLE, (byte) -1);
        for (int i = 0; i < 26; i++) {
            TABLE['A' + i] = (byte) i;
            TABLE['a' + i] = (byte) (26 + i);
        }
        for (int i = 0; i < 10; i++) TABLE['0' + i] = (byte) (52 + i);
        TABLE['-'] = 62;
        TABLE['_'] = 63;
    }

    /** Number of data characters in {@code b[s, e)} (without padding), or -1 if the length or padding is impossible. */
    static int dataLength(byte[] b, int s, int e) {
        int n = e - s;
        int pad = 0;
        if (n > 0 && b[e - 1] == '=') {
            pad = n > 1 && b[e - 2] == '=' ? 2 : 1;
            if ((n & 3) != 0) return -1;
        }
        int m = n - pad;
        int r = m & 3;
        if (r == 1) return -1;
        if (pad == 1 && r != 3) return -1;
        if (pad == 2 && r != 2) return -1;
        return m;
    }

    /** Decoded byte count for {@code m} data characters. */
    static int decodedLength(int m) {
        int r = m & 3;
        return (m >> 2) * 3 + (r == 0 ? 0 : r - 1);
    }

    /**
     * Decodes {@code m} data characters starting at {@code s} (from {@link #dataLength}) into {@code dst[d]}.
     * Returns the decoded length, or -1 on a character outside the alphabet. With {@code canonical}, unused low bits
     * of the last character must be zero.
     */
    static int decode(byte[] b, int s, int m, byte[] dst, int d, boolean canonical) {
        int p = s;
        int full = s + (m & ~3);
        int o = d;
        while (p < full) {
            int c0 = TABLE[b[p] & 0xFF];
            int c1 = TABLE[b[p + 1] & 0xFF];
            int c2 = TABLE[b[p + 2] & 0xFF];
            int c3 = TABLE[b[p + 3] & 0xFF];
            if ((c0 | c1 | c2 | c3) < 0) return -1;
            int v = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
            dst[o] = (byte) (v >> 16);
            dst[o + 1] = (byte) (v >> 8);
            dst[o + 2] = (byte) v;
            o += 3;
            p += 4;
        }
        int r = m & 3;
        if (r == 2) {
            int c0 = TABLE[b[p] & 0xFF];
            int c1 = TABLE[b[p + 1] & 0xFF];
            if ((c0 | c1) < 0 || (canonical && (c1 & 0x0F) != 0)) return -1;
            dst[o++] = (byte) ((c0 << 2) | (c1 >> 4));
        } else if (r == 3) {
            int c0 = TABLE[b[p] & 0xFF];
            int c1 = TABLE[b[p + 1] & 0xFF];
            int c2 = TABLE[b[p + 2] & 0xFF];
            if ((c0 | c1 | c2) < 0 || (canonical && (c2 & 0x03) != 0)) return -1;
            dst[o++] = (byte) ((c0 << 2) | (c1 >> 4));
            dst[o++] = (byte) ((c1 << 4) | (c2 >> 2));
        }
        return o - d;
    }
}
