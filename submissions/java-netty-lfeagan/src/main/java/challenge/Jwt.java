package challenge;

import io.netty.buffer.ByteBuf;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.Map;

/**
 * HS256 JWT verification. Every token's signature is checked on every request (rule 5); the only
 * thing kept between requests is the keyed Mac and scratch arrays.
 *
 * Not thread-safe: one instance per event loop thread.
 */
final class Jwt {
    static final int OK = 0, INVALID = 1, BAD_PAYLOAD = 2;

    private static final int MAX_TOKEN = 4096;

    private final Mac mac;
    private final byte[] token = new byte[MAX_TOKEN];
    private final byte[] decoded = new byte[MAX_TOKEN];
    private final byte[] expected = new byte[32];

    /** Set by {@link #verify} when it returns OK. */
    long userId;
    String username;

    Jwt(String secret) {
        try {
            mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /** Verifies the token in buf[start, end) (the part after "Bearer "). */
    int verify(ByteBuf buf, int start, int end) {
        int len = end - start;
        if (len <= 0 || len > MAX_TOKEN) return INVALID;
        buf.getBytes(start, token, 0, len);
        int dot1 = -1, dot2 = -1;
        for (int i = 0; i < len; i++) {
            if (token[i] == '.') {
                if (dot1 < 0) dot1 = i;
                else if (dot2 < 0) dot2 = i;
                else return INVALID;
            }
        }
        if (dot2 < 0) return INVALID;

        // header: must say HS256
        int n = base64url(token, 0, dot1, decoded);
        if (n < 0) return INVALID;
        Map<?, ?> header;
        try {
            if (!(Json.parse(decoded, 0, n) instanceof Map<?, ?> m)) return INVALID;
            header = m;
        } catch (Json.ParseException e) {
            return INVALID;
        }
        if (!"HS256".equals(header.get("alg"))) return INVALID;

        // signature, compared in constant time
        n = base64url(token, dot2 + 1, len, decoded);
        if (n != 32) return INVALID;
        try {
            mac.update(token, 0, dot2);
            mac.doFinal(expected, 0);
        } catch (GeneralSecurityException e) {
            return INVALID;
        }
        int diff = 0;
        for (int i = 0; i < 32; i++) diff |= expected[i] ^ decoded[i];
        if (diff != 0) return INVALID;

        // payload: exp (and nbf, if present), then sub and username
        n = base64url(token, dot1 + 1, dot2, decoded);
        if (n < 0) return INVALID;
        Map<?, ?> claims;
        try {
            if (!(Json.parse(decoded, 0, n) instanceof Map<?, ?> m)) return INVALID;
            claims = m;
        } catch (Json.ParseException e) {
            return INVALID;
        }
        double now = System.currentTimeMillis() / 1000.0;
        Object exp = claims.get("exp");
        if (exp != null && !(exp instanceof Double d && now < d)) return INVALID;
        Object nbf = claims.get("nbf");
        if (nbf != null && !(nbf instanceof Double d && now >= d)) return INVALID;

        if (!(claims.get("sub") instanceof String sub) || !(claims.get("username") instanceof String name)) return BAD_PAYLOAD;
        long id = parsePositive(sub);
        if (id <= 0) return BAD_PAYLOAD;
        userId = id;
        username = name;
        return OK;
    }

    /** A positive decimal integer of at most 18 digits, or -1. */
    static long parsePositive(CharSequence s) {
        int len = s.length();
        if (len == 0 || len > 18) return -1;
        long v = 0;
        for (int i = 0; i < len; i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v > 0 ? v : -1;
    }

    private static final byte[] B64 = new byte[128];
    static {
        java.util.Arrays.fill(B64, (byte) -1);
        String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; i++) B64[alphabet.charAt(i)] = (byte) i;
    }

    /** Decodes unpadded base64url src[from, to) into dst; returns the length, or -1 if invalid. */
    private static int base64url(byte[] src, int from, int to, byte[] dst) {
        int len = to - from;
        if (len % 4 == 1) return -1;
        int out = 0, acc = 0, bits = 0;
        for (int i = from; i < to; i++) {
            int c = src[i];
            if (c < 0 || B64[c] < 0) return -1;
            acc = acc << 6 | B64[c];
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                dst[out++] = (byte) (acc >> bits);
            }
        }
        return out;
    }
}
