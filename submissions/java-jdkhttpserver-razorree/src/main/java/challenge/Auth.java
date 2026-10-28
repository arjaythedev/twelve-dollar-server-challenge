package challenge;

import java.io.IOException;
import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.ArrayBlockingQueue;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class Auth {
    record User(long id, String username) {}
    private final SecretKeySpec key;
    private final ArrayBlockingQueue<Mac> macs = new ArrayBlockingQueue<>(64);

    Auth(String secret) { key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"); }

    User verify(String authorization) throws Server.ApiError {
        if (authorization == null || !authorization.startsWith("Bearer "))
            throw new Server.ApiError(401, "missing bearer token");
        Map<?, ?> payload;
        try {
            String[] parts = authorization.substring(7).split("\\.", -1);
            if (parts.length != 3) throw new IOException("JWT segments");
            Object header = Json.parse(decode(parts[0]));
            if (!(header instanceof Map<?, ?> h) || !"HS256".equals(h.get("alg")))
                throw new IOException("JWT algorithm");
            Mac mac = macs.poll();
            if (mac == null) {
                mac = Mac.getInstance("HmacSHA256");
                mac.init(key);
            }
            byte[] expected;
            try {
                expected = mac.doFinal((parts[0] + "." + parts[1]).getBytes(StandardCharsets.US_ASCII));
            } finally {
                mac.reset();
                macs.offer(mac);
            }
            if (!MessageDigest.isEqual(expected, decode(parts[2]))) throw new IOException("JWT signature");
            Object parsed = Json.parse(decode(parts[1]));
            if (!(parsed instanceof Map<?, ?> p)) throw new IOException("JWT payload");
            payload = p;
            BigDecimal now = BigDecimal.valueOf(Instant.now().getEpochSecond());
            if (payload.containsKey("exp") && (!(payload.get("exp") instanceof BigDecimal exp) || now.compareTo(exp) >= 0))
                throw new IOException("JWT expired");
            if (payload.containsKey("nbf") && (!(payload.get("nbf") instanceof BigDecimal nbf) || now.compareTo(nbf) < 0))
                throw new IOException("JWT not active");
        } catch (IOException | IllegalArgumentException | GeneralSecurityException e) {
            throw new Server.ApiError(401, "invalid or expired token");
        }
        Object subject = payload.get("sub"), username = payload.get("username");
        long id;
        try {
            if (!(subject instanceof String s) || !Server.decimalId(s) || !(username instanceof String))
                throw new IllegalArgumentException();
            id = Long.parseLong(s);
            if (id <= 0) throw new IllegalArgumentException();
        } catch (IllegalArgumentException e) {
            throw new Server.ApiError(401, "invalid token payload");
        }
        return new User(id, (String) username);
    }

    private static byte[] decode(String value) {
        if (value.isEmpty()) throw new IllegalArgumentException("Empty base64url");
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (!(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '_'))
                throw new IllegalArgumentException("Invalid base64url");
        }
        return Base64.getUrlDecoder().decode(value);
    }
}
