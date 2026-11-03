package challenge;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class Jwt {
    static final int OK = 0, MISSING = 1, INVALID = 2, BAD_PAYLOAD = 3;

    private static final byte[] BEARER = Out.ascii("Bearer ");
    private static final byte[] B64URL = Out.ascii("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");

    private final Mac mac;
    private final byte[] digest = new byte[32];
    private final byte[] sig = new byte[43];

    long userId;
    byte[] username;

    Jwt(String secret) throws GeneralSecurityException {
        mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
    }

    int verify(byte[] b, int start, int end) {
        if (start < 0 || end - start < BEARER.length) return MISSING;
        for (int k = 0; k < BEARER.length; k++) if (b[start + k] != BEARER[k]) return MISSING;
        int t = start + BEARER.length;
        int d1 = -1, d2 = -1;
        for (int k = t; k < end; k++) {
            if (b[k] != '.') continue;
            if (d1 < 0) d1 = k;
            else if (d2 < 0) d2 = k;
            else return INVALID;
        }
        if (d2 < 0) return INVALID;
        try {
            Object header = Json.parse(decode(b, t, d1));
            if (!"HS256".equals(Json.get(header, "alg"))) return INVALID;
            if (!signatureMatches(b, t, d2, end)) return INVALID;
            Object payload = Json.parse(decode(b, d1 + 1, d2));
            if (!(payload instanceof java.util.Map)) return INVALID;
            long now = System.currentTimeMillis() / 1000;
            Object exp = Json.get(payload, "exp");
            if (exp != null && exp != Json.NULL && !(exp instanceof Double e && now < e)) return INVALID;
            Object nbf = Json.get(payload, "nbf");
            if (nbf != null && nbf != Json.NULL && !(nbf instanceof Double n && now >= n)) return INVALID;
            Object sub = Json.get(payload, "sub");
            Object name = Json.get(payload, "username");
            long id = sub instanceof String s ? positiveInt(s) : -1;
            if (id <= 0 || !(name instanceof String n)) return BAD_PAYLOAD;
            userId = id;
            username = n.getBytes(StandardCharsets.UTF_8);
            return OK;
        } catch (Json.Malformed | IllegalArgumentException e) {
            return INVALID;
        }
    }

    private static String decode(byte[] b, int from, int to) throws Json.Malformed {
        for (int k = from; k < to; k++) if (b64index(b[k]) < 0) throw Json.Malformed.INSTANCE;
        byte[] raw = Base64.getUrlDecoder().decode(java.util.Arrays.copyOfRange(b, from, to));
        return Json.utf8(raw, 0, raw.length);
    }

    private static int b64index(byte c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    }

    private boolean signatureMatches(byte[] b, int from, int dot, int end) {
        mac.update(b, from, dot - from);
        try {
            mac.doFinal(digest, 0);
        } catch (javax.crypto.ShortBufferException e) {
            throw new IllegalStateException(e);
        }
        int o = 0;
        for (int k = 0; k < 30; k += 3) {
            int v = (digest[k] & 0xff) << 16 | (digest[k + 1] & 0xff) << 8 | (digest[k + 2] & 0xff);
            sig[o++] = B64URL[v >>> 18];
            sig[o++] = B64URL[(v >>> 12) & 63];
            sig[o++] = B64URL[(v >>> 6) & 63];
            sig[o++] = B64URL[v & 63];
        }
        int v = (digest[30] & 0xff) << 8 | (digest[31] & 0xff);
        sig[o++] = B64URL[v >>> 10];
        sig[o++] = B64URL[(v >>> 4) & 63];
        sig[o] = B64URL[(v << 2) & 63];
        if (end - dot - 1 != sig.length) return false;
        int diff = 0;
        for (int k = 0; k < sig.length; k++) diff |= sig[k] ^ b[dot + 1 + k];
        return diff == 0;
    }

    static long positiveInt(CharSequence s) {
        int n = s.length();
        if (n == 0 || n > 19) return -1;
        long v = 0;
        for (int k = 0; k < n; k++) {
            char c = s.charAt(k);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
            if (v < 0) return -1;
        }
        return v > 0 ? v : -1;
    }
}
