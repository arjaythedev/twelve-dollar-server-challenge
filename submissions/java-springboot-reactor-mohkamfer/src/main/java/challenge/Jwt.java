package challenge;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.Map;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** HS256-only JWT verification, done by hand. Every call verifies the signature: nothing is cached. */
final class Jwt {

    record Auth(long userId, String username) {}

    private static final ApiError MISSING_TOKEN = new ApiError(Resp.MISSING_TOKEN);
    private static final ApiError INVALID_TOKEN = new ApiError(Resp.INVALID_TOKEN);
    private static final ApiError INVALID_PAYLOAD = new ApiError(Resp.INVALID_PAYLOAD);
    private static final Base64.Decoder B64 = Base64.getUrlDecoder();

    private final ThreadLocal<Mac> mac;

    Jwt(String secret) {
        SecretKeySpec key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
        this.mac = ThreadLocal.withInitial(() -> {
            try {
                Mac m = Mac.getInstance("HmacSHA256");
                m.init(key);
                return m;
            } catch (GeneralSecurityException e) {
                throw new IllegalStateException(e);
            }
        });
    }

    /** Returns the authenticated user, or throws an {@link ApiError} carrying the right 401. */
    Auth verify(String header) {
        if (header == null || !header.startsWith("Bearer ")) throw MISSING_TOKEN;
        String token = header.substring(7);
        int d1 = token.indexOf('.');
        int d2 = d1 < 0 ? -1 : token.indexOf('.', d1 + 1);
        if (d1 <= 0 || d2 < 0 || token.indexOf('.', d2 + 1) >= 0) throw INVALID_TOKEN;

        Map<?, ?> head = decodeObject(token.substring(0, d1));
        if (!"HS256".equals(head.get("alg"))) throw INVALID_TOKEN;

        byte[] sig;
        try {
            sig = B64.decode(token.substring(d2 + 1));
        } catch (IllegalArgumentException e) {
            throw INVALID_TOKEN;
        }
        byte[] expected = mac.get().doFinal(token.substring(0, d2).getBytes(StandardCharsets.US_ASCII));
        if (!MessageDigest.isEqual(expected, sig)) throw INVALID_TOKEN;

        Map<?, ?> payload = decodeObject(token.substring(d1 + 1, d2));
        double now = System.currentTimeMillis() / 1000.0;
        Object exp = payload.get("exp");
        if (exp != null && !(exp instanceof Double e && now < e)) throw INVALID_TOKEN;
        Object nbf = payload.get("nbf");
        if (nbf != null && !(nbf instanceof Double n && now >= n)) throw INVALID_TOKEN;

        long userId = payload.get("sub") instanceof String sub ? Ids.parsePositiveLong(sub) : -1;
        if (userId <= 0 || !(payload.get("username") instanceof String username)) throw INVALID_PAYLOAD;
        return new Auth(userId, username);
    }

    private static Map<?, ?> decodeObject(String part) {
        try {
            Object v = Json.parse(new String(B64.decode(part), StandardCharsets.UTF_8));
            if (v instanceof Map<?, ?> m) return m;
        } catch (IllegalArgumentException | Json.Malformed ignored) {
            // fall through
        }
        throw INVALID_TOKEN;
    }
}
