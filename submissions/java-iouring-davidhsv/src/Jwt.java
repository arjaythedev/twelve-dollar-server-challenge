import java.lang.foreign.Arena;

/** HS256: base64url, SHA-256 and HMAC, and the claims. Every token's signature is checked on
 * every request; what is computed once is the key schedule (the SHA-256 states after the key's
 * two padded blocks), not a verification. */
final class Jwt {
    private Jwt() {}

    private static final int[] K = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    private static final int[] INIT = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    private static final int[] inner = new int[8];  // the key schedule
    private static final int[] outer = new int[8];
    private static final int[] h = new int[8];
    private static final int[] w = new int[64];
    private static final byte[] B64 = new byte[256];
    // Native scratch: the last one or two blocks of a hash, the inner digest, the expected MAC.
    private static final long LAST, DIGEST, WANT;
    static {
        long at = Arena.global().allocate(256, 16).address();
        LAST = at;
        DIGEST = at + 128;
        WANT = at + 192;
        java.util.Arrays.fill(B64, (byte) 0xff);
        String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; i++) B64[alphabet.charAt(i)] = (byte) i;
    }

    static final Buf head = new Buf(1024);   // the token's decoded header (first its signature)
    static final Buf body = new Buf(1024);   // and payload
    /** The verified token's username. */
    static final Buf who = new Buf(256);
    /** The verified token's user id. */
    static long uid;

    /** One SHA-256 compression of the 64 bytes at a. */
    private static void block(int[] s, long a) {
        for (int i = 0; i < 16; i++) w[i] = N.M.get(N.INT_BE, a + i * 4L);
        for (int i = 16; i < 64; i++) {
            int x = w[i - 15], y = w[i - 2];
            int s0 = Integer.rotateRight(x, 7) ^ Integer.rotateRight(x, 18) ^ (x >>> 3);
            int s1 = Integer.rotateRight(y, 17) ^ Integer.rotateRight(y, 19) ^ (y >>> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        int a0 = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], hh = s[7];
        for (int i = 0; i < 64; i++) {
            int t1 = hh + (Integer.rotateRight(e, 6) ^ Integer.rotateRight(e, 11) ^ Integer.rotateRight(e, 25))
                    + ((e & f) ^ (~e & g)) + K[i] + w[i];
            int t2 = (Integer.rotateRight(a0, 2) ^ Integer.rotateRight(a0, 13) ^ Integer.rotateRight(a0, 22))
                    + ((a0 & b) ^ (a0 & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a0;
            a0 = t1 + t2;
        }
        s[0] += a0;
        s[1] += b;
        s[2] += c;
        s[3] += d;
        s[4] += e;
        s[5] += f;
        s[6] += g;
        s[7] += hh;
    }

    /** Finishes a hash whose state s has taken `before` bytes already, with the n bytes at msg as
     * the rest; the digest goes to the 32 bytes at out. */
    private static void finish(int[] s, long msg, long n, long before, long out) {
        long at = 0;
        while (at + 64 <= n) { block(s, msg + at); at += 64; }
        for (int k = 0; k < 128; k += 8) N.M.set(N.LONG_U, LAST + k, 0L);
        int rest = (int) (n - at);
        N.copy(msg + at, LAST, rest);
        N.put(LAST + rest, 0x80);
        int total = rest < 56 ? 64 : 128;
        long bits = (before + n) * 8;
        for (int i = 0; i < 8; i++) N.put(LAST + total - 1 - i, (int) (bits >>> (i * 8)));
        block(s, LAST);
        if (total == 128) block(s, LAST + 64);
        for (int i = 0; i < 8; i++) N.M.set(N.INT_BE, out + i * 4L, s[i]);
    }

    /** Hashes the key's two padded blocks once: every HMAC then starts from these states. */
    static void key(byte[] secret) {
        long kb = Arena.global().allocate(64, 16).address();
        for (int i = 0; i < 64; i++) N.put(kb + i, 0);
        if (secret.length > 64) {
            long m = Arena.global().allocate(secret.length, 16).address();
            N.copy(secret, 0, m, secret.length);
            System.arraycopy(INIT, 0, h, 0, 8);
            finish(h, m, secret.length, 0, kb);
        } else {
            N.copy(secret, 0, kb, secret.length);
        }
        long pad = Arena.global().allocate(64, 16).address();
        for (int i = 0; i < 64; i++) N.put(pad + i, N.u(kb + i) ^ 0x36);
        System.arraycopy(INIT, 0, inner, 0, 8);
        block(inner, pad);
        for (int i = 0; i < 64; i++) N.put(pad + i, N.u(kb + i) ^ 0x5c);
        System.arraycopy(INIT, 0, outer, 0, 8);
        block(outer, pad);
    }

    private static void hmac(long msg, long n, long out) {
        System.arraycopy(inner, 0, h, 0, 8);
        finish(h, msg, n, 64, DIGEST);
        System.arraycopy(outer, 0, h, 0, 8);
        finish(h, DIGEST, 32, 64, out);
    }

    /** Decodes the n bytes of unpadded base64url at s, appending to out. Only the canonical
     * spelling is accepted (the unused bits of the last character zero), so one spelling of a
     * signature verifies. */
    static boolean base64url(long s, long n, Buf out) {
        long tail = n % 4;
        if (tail == 1) return false;
        long whole = n - tail;
        out.ensure(n / 4 * 3 + 3);
        long o = out.base + out.len;
        long i = 0;
        while (i < whole) {
            int a = B64[N.u(s + i)], b = B64[N.u(s + i + 1)], c = B64[N.u(s + i + 2)], d = B64[N.u(s + i + 3)];
            if (((a | b | c | d) & 0xc0) != 0) return false;
            int v = (a << 18) | (b << 12) | (c << 6) | d;
            N.put(o, v >> 16);
            N.put(o + 1, v >> 8);
            N.put(o + 2, v);
            o += 3;
            i += 4;
        }
        if (tail != 0) {
            int a = B64[N.u(s + i)], b = B64[N.u(s + i + 1)];
            int c = tail == 3 ? B64[N.u(s + i + 2)] : 0;
            if (((a | b | c) & 0xc0) != 0) return false;
            int v = (a << 18) | (b << 12) | (c << 6);
            if ((tail == 2 ? v & 0xffff : v & 0xff) != 0) return false;
            N.put(o++, v >> 16);
            if (tail == 3) N.put(o++, v >> 8);
        }
        out.len = o - out.base;
        return true;
    }

    private static long find(long a, long end, int c) {
        for (long p = a; p < end; p++) if (N.u(p) == c) return p;
        return -1;
    }

    private static final byte[] BEARER = Out.ascii("Bearer ");
    private static final byte[] ALG = Out.ascii("alg");
    private static final byte[] HS256 = Out.ascii("HS256");
    private static final byte[] EXP = Out.ascii("exp");
    private static final byte[] SUB = Out.ascii("sub");
    private static final byte[] USERNAME = Out.ascii("username");

    /** Verifies an Authorization header value (the n bytes at a). 0 with uid and who set; or 1
     * no bearer token, 2 invalid or expired, 3 a bad payload. */
    static int verify(long a, long n, boolean present) {
        if (!present || !Out.startsWith(a, n, BEARER)) return 1;
        long token = a + 7, end = a + n;
        long dot1 = find(token, end, '.');
        if (dot1 < 0) return 2;
        long dot2 = find(dot1 + 1, end, '.');
        if (dot2 < 0) return 2;
        long sig = dot2 + 1;
        if (find(sig, end, '.') >= 0) return 2;
        head.clear();
        if (!base64url(sig, end - sig, head) || head.len != 32) return 2;
        hmac(token, dot2 - token, WANT);
        int diff = 0;
        for (int i = 0; i < 32; i++) diff |= N.u(WANT + i) ^ N.u(head.base + i);  // every byte, whatever the first difference
        if (diff != 0) return 2;

        head.clear();
        if (!base64url(token, dot1 - token, head)) return 2;
        if (Json.members(head.base, head.base + head.len, 1, ALG, null, null) != 1 || Json.kind[0] != Json.STRING) return 2;
        who.clear();
        Json.text(0, who);
        if (!who.is(HS256)) return 2;

        body.clear();
        if (!base64url(dot1 + 1, dot2 - dot1 - 1, body)) return 2;
        if (Json.members(body.base, body.base + body.len, 3, EXP, SUB, USERNAME) != 1) return 2;
        if (Json.kind[0] != Json.NUMBER) return 2;
        double until = parseFloat(Json.s[0], Json.e[0]);
        if (!floatOk || !(N.timeNs() / 1000000000.0 < until)) return 2;
        if (Json.kind[1] != Json.STRING || Json.kind[2] != Json.STRING) return 3;
        who.clear();
        Json.text(1, who);
        long id = Api.positiveInt(who.base, who.len);
        if (id <= 0) return 3;
        who.clear();
        Json.text(2, who);
        uid = id;
        return 0;
    }

    private static final double[] POW10 = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17,
        1e18, 1e19, 1e20, 1e21, 1e22,
    };
    private static boolean floatOk;

    /** A JSON number (s..e, already validated) as the nearest double. The shapes a token's exp
     * takes (an integer, or a short decimal fraction) are exact in one rounding; anything else
     * goes to Double.parseDouble, which also rounds correctly. */
    static double parseFloat(long s, long e) {
        floatOk = true;
        long i = s;
        boolean neg = false;
        if (i < e && N.u(i) == '-') { neg = true; i++; }
        long start = i, mant = 0;
        while (i < e && N.u(i) - '0' >= 0 && N.u(i) - '0' < 10) { mant = mant * 10 + (N.u(i) - '0'); i++; }
        long digits = i - start, frac = 0;
        if (i < e && N.u(i) == '.') {
            i++;
            long f0 = i;
            while (i < e && N.u(i) - '0' >= 0 && N.u(i) - '0' < 10) { mant = mant * 10 + (N.u(i) - '0'); i++; }
            frac = i - f0;
            digits += frac;
        }
        if (digits == 0) { floatOk = false; return 0; }
        if (digits <= 19 && i == e && mant >>> 63 == 0 && (frac == 0 || mant <= 9007199254740992L)) {
            double v = (double) mant;
            if (frac > 0) v /= POW10[(int) frac];
            return neg ? -v : v;
        }
        byte[] text = new byte[(int) (e - s)];
        for (int k = 0; k < text.length; k++) text[k] = N.b(s + k);
        try {
            return Double.parseDouble(new String(text, java.nio.charset.StandardCharsets.US_ASCII));
        } catch (NumberFormatException x) {
            floatOk = false;
            return 0;
        }
    }
}
