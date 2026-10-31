package feed.auth;

import feed.json.JsonScanner;
import java.nio.charset.StandardCharsets;
import java.security.DigestException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;

/**
 * HS256 JWT verification for {@code Authorization: Bearer <jwt>} headers. Every call verifies the signature and
 * claims from scratch; nothing is cached across calls.
 *
 * <p>Single-threaded. Internal buffers are reused and grow only for larger tokens, so the success path does not
 * allocate once warmed up. HMAC-SHA256 (RFC 2104) is computed with one reused {@link MessageDigest} and precomputed
 * key pads instead of {@code javax.crypto.Mac}, whose {@code doFinal} allocates a new array on every call.
 */
public final class JwtVerifier {
    public static final int OK = 0, MISSING_BEARER = 1, INVALID_TOKEN = 2, INVALID_PAYLOAD = 3;

    private static final int SHA256_BLOCK = 64;
    private static final int SHA256_LEN = 32;

    private static final byte[] HS256 = ascii("HS256");
    private static final int P_EXP = 0, P_NBF = 1, P_SUB = 2, P_USERNAME = 3;

    private final JsonScanner headerScanner = new JsonScanner(ascii("alg"));
    private final JsonScanner payloadScanner =
            new JsonScanner(ascii("exp"), ascii("nbf"), ascii("sub"), ascii("username"));
    private final MessageDigest sha256;
    private final byte[] ipad = new byte[SHA256_BLOCK];
    private final byte[] opad = new byte[SHA256_BLOCK];
    private final byte[] mac = new byte[SHA256_LEN];
    private final byte[] sig = new byte[SHA256_LEN];
    private byte[] token = new byte[512];
    private byte[] decoded = new byte[512];
    private byte[] scratch = new byte[64];
    private byte[] username = new byte[64];
    private int usernameLength;
    private long userId;

    public JwtVerifier(byte[] secretUtf8) {
        try {
            sha256 = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
        byte[] key = secretUtf8.length > SHA256_BLOCK ? sha256.digest(secretUtf8) : secretUtf8;
        for (int i = 0; i < SHA256_BLOCK; i++) {
            byte k = i < key.length ? key[i] : 0;
            ipad[i] = (byte) (k ^ 0x36);
            opad[i] = (byte) (k ^ 0x5c);
        }
    }

    /** Verifies an {@code Authorization} header value ({@code null} if absent) at time {@code nowEpochSeconds}. */
    public int verify(CharSequence authorizationHeader, long nowEpochSeconds) {
        userId = 0;
        usernameLength = 0;
        CharSequence h = authorizationHeader;
        if (h == null) return MISSING_BEARER;
        int n = h.length();
        if (n < 7
                || h.charAt(0) != 'B'
                || h.charAt(1) != 'e'
                || h.charAt(2) != 'a'
                || h.charAt(3) != 'r'
                || h.charAt(4) != 'e'
                || h.charAt(5) != 'r'
                || h.charAt(6) != ' ') {
            return MISSING_BEARER;
        }
        int len = n - 7;
        if (token.length < len) token = new byte[Math.max(len, token.length * 2)];
        byte[] t = token;
        for (int i = 0; i < len; i++) {
            char c = h.charAt(7 + i);
            if (c > 0x7F) return INVALID_TOKEN;
            t[i] = (byte) c;
        }
        return verifyToken(t, 0, len, nowEpochSeconds);
    }

    /** {@code sub} of the last token that verified OK. */
    public long userId() {
        return userId;
    }

    /** UTF-8 of the decoded {@code username} claim after OK; valid until the next verify. */
    public byte[] username() {
        return username;
    }

    public int usernameLength() {
        return usernameLength;
    }

    private int verifyToken(byte[] b, int off, int len, long now) {
        int end = off + len;
        int d1 = -1;
        int d2 = -1;
        for (int i = off; i < end; i++) {
            if (b[i] == '.') {
                if (d1 < 0) {
                    d1 = i;
                } else if (d2 < 0) {
                    d2 = i;
                } else {
                    return INVALID_TOKEN;
                }
            }
        }
        if (d2 < 0 || d1 == off || d2 == d1 + 1) return INVALID_TOKEN;

        int hm = Base64Url.dataLength(b, off, d1);
        int pm = Base64Url.dataLength(b, d1 + 1, d2);
        int sm = Base64Url.dataLength(b, d2 + 1, end);
        if (hm < 0 || pm < 0 || sm < 0) return INVALID_TOKEN;
        if (Base64Url.decodedLength(sm) != SHA256_LEN) return INVALID_TOKEN;
        int need = Math.max(Base64Url.decodedLength(hm), Base64Url.decodedLength(pm));
        if (decoded.length < need) decoded = new byte[Math.max(need, decoded.length * 2)];
        byte[] dec = decoded;

        int hl = Base64Url.decode(b, off, hm, dec, 0, false);
        if (hl < 0 || !headerScanner.scan(dec, 0, hl)) return INVALID_TOKEN;
        if (headerScanner.topType() != JsonScanner.OBJECT || !stringEquals(headerScanner, 0, dec, HS256)) {
            return INVALID_TOKEN;
        }

        if (Base64Url.decode(b, d2 + 1, sm, sig, 0, true) != SHA256_LEN) return INVALID_TOKEN;
        hmac(b, off, d2 - off);
        if (!MessageDigest.isEqual(mac, sig)) return INVALID_TOKEN;

        int pl = Base64Url.decode(b, d1 + 1, pm, dec, 0, false);
        JsonScanner ps = payloadScanner;
        if (pl < 0 || !ps.scan(dec, 0, pl) || ps.topType() != JsonScanner.OBJECT) return INVALID_TOKEN;
        if (ps.type(P_EXP) != JsonScanner.NUMBER
                || !JsonScanner.numberGreaterThan(dec, ps.start(P_EXP), ps.end(P_EXP), now)) {
            return INVALID_TOKEN;
        }
        if (ps.type(P_NBF) == JsonScanner.NUMBER
                && JsonScanner.numberGreaterThan(dec, ps.start(P_NBF), ps.end(P_NBF), now)) {
            return INVALID_TOKEN;
        }

        if (ps.type(P_SUB) != JsonScanner.STRING || ps.type(P_USERNAME) != JsonScanner.STRING) return INVALID_PAYLOAD;
        long id = parseSub(dec, ps.start(P_SUB), ps.end(P_SUB), ps.escaped(P_SUB));
        if (id < 1) return INVALID_PAYLOAD;

        int us = ps.start(P_USERNAME);
        int ue = ps.end(P_USERNAME);
        if (username.length < ue - us) username = new byte[Math.max(ue - us, username.length * 2)];
        int ul;
        if (ps.escaped(P_USERNAME)) {
            ul = JsonScanner.decodeString(dec, us, ue, username, 0);
        } else {
            ul = ue - us;
            System.arraycopy(dec, us, username, 0, ul);
        }
        userId = id;
        usernameLength = ul;
        return OK;
    }

    /** Positive integer value of a string of ASCII digits that fits in a long, else -1. */
    private long parseSub(byte[] b, int s, int e, boolean escaped) {
        if (escaped) {
            if (scratch.length < e - s) scratch = new byte[Math.max(e - s, scratch.length * 2)];
            e = JsonScanner.decodeString(b, s, e, scratch, 0);
            s = 0;
            b = scratch;
        }
        if (s == e) return -1;
        long v = 0;
        for (int p = s; p < e; p++) {
            int d = b[p] - '0';
            if (d < 0 || d > 9) return -1;
            if (v > (Long.MAX_VALUE - d) / 10) return -1;
            v = v * 10 + d;
        }
        return v >= 1 ? v : -1;
    }

    private boolean stringEquals(JsonScanner sc, int slot, byte[] src, byte[] expected) {
        if (sc.type(slot) != JsonScanner.STRING) return false;
        int s = sc.start(slot);
        int e = sc.end(slot);
        if (sc.escaped(slot)) {
            if (scratch.length < e - s) scratch = new byte[Math.max(e - s, scratch.length * 2)];
            int n = JsonScanner.decodeString(src, s, e, scratch, 0);
            return Arrays.equals(scratch, 0, n, expected, 0, expected.length);
        }
        return Arrays.equals(src, s, e, expected, 0, expected.length);
    }

    private void hmac(byte[] b, int off, int len) {
        MessageDigest md = sha256;
        try {
            md.update(ipad, 0, SHA256_BLOCK);
            md.update(b, off, len);
            md.digest(mac, 0, SHA256_LEN);
            md.update(opad, 0, SHA256_BLOCK);
            md.update(mac, 0, SHA256_LEN);
            md.digest(mac, 0, SHA256_LEN);
        } catch (DigestException e) {
            throw new IllegalStateException(e);
        }
    }

    private static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.US_ASCII);
    }
}
