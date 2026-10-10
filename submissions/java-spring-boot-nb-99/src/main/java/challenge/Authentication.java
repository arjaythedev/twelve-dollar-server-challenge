package challenge;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.time.Instant;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;
import tools.jackson.databind.JsonNode;
import tools.jackson.databind.json.JsonMapper;

@Component
final class Authentication {
    private final SecretKeySpec key;
    private final JsonMapper json;

    Authentication(@Value("${JWT_SECRET}") String secret, JsonMapper json) {
        if (secret.isEmpty()) {
            throw new IllegalArgumentException("JWT_SECRET must not be empty");
        }
        key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
        this.json = json;
    }

    record User(long id, String username) {}

    User authenticate(String authorization) {
        if (authorization == null || !authorization.startsWith("Bearer ")) {
            throw new ApiException(401, "missing bearer token");
        }
        JsonNode payload;
        try {
            String[] parts = authorization.substring(7).split("\\.", -1);
            if (parts.length != 3
                    || parts[0].isEmpty()
                    || parts[1].isEmpty()
                    || parts[2].isEmpty()) {
                throw new IllegalArgumentException();
            }
            Base64.Decoder decoder = Base64.getUrlDecoder();
            JsonNode header = json.readTree(decoder.decode(parts[0]));
            if (!header.isObject()
                    || !"HS256".equals(header.path("alg").asString())
                    || header.has("crit")
                    || header.has("b64")) {
                throw new IllegalArgumentException();
            }
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(key);
            byte[] expected =
                    mac.doFinal((parts[0] + "." + parts[1]).getBytes(StandardCharsets.US_ASCII));
            if (!MessageDigest.isEqual(expected, decoder.decode(parts[2]))) {
                throw new IllegalArgumentException();
            }
            payload = json.readTree(decoder.decode(parts[1]));
            JsonNode expiration = payload.path("exp");
            if (!payload.isObject()
                    || !expiration.isNumber()
                    || expiration.asDouble() <= Instant.now().getEpochSecond()) {
                throw new IllegalArgumentException();
            }
        } catch (GeneralSecurityException | RuntimeException exception) {
            throw new ApiException(401, "invalid or expired token");
        }
        JsonNode subject = payload.path("sub");
        JsonNode username = payload.path("username");
        if (!subject.isString() || !username.isString()) {
            throw new ApiException(401, "invalid token payload");
        }
        try {
            return new User(positiveInteger(subject.asString()), username.asString());
        } catch (IllegalArgumentException exception) {
            throw new ApiException(401, "invalid token payload");
        }
    }

    static long positiveInteger(String text) {
        if (!text.matches("[0-9]+")) {
            throw new IllegalArgumentException();
        }
        long value = Long.parseLong(text);
        if (value <= 0) {
            throw new IllegalArgumentException();
        }
        return value;
    }
}
