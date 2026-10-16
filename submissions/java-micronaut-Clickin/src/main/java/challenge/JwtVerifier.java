package challenge;

import io.micronaut.json.JsonMapper;
import io.micronaut.json.tree.JsonNode;
import jakarta.inject.Singleton;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Base64;

@Singleton
public final class JwtVerifier {
    public record User(long id, String username) {}
    private final JsonMapper json;
    private final ThreadLocal<Mac> hmac;
    public JwtVerifier(JsonMapper json) {
        this(json, System.getenv().getOrDefault("JWT_SECRET", "twelve-dollar-challenge"));
    }
    JwtVerifier(JsonMapper json, String secret) {
        this.json = json;
        if (secret.isEmpty()) throw new IllegalArgumentException("JWT_SECRET must not be empty");
        var key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
        hmac = ThreadLocal.withInitial(() -> {
            try {
                Mac mac = Mac.getInstance("HmacSHA256");
                mac.init(key);
                return mac;
            } catch (GeneralSecurityException e) {
                throw new IllegalStateException("HmacSHA256 unavailable", e);
            }
        });
    }
    public User verify(String authorization) {
        if (authorization == null || !authorization.startsWith("Bearer "))
            throw new ApiException(401, "missing bearer token");
        JsonNode payload;
        try {
            int first = authorization.indexOf('.', 7);
            int second = first < 0 ? -1 : authorization.indexOf('.', first + 1);
            if (second < 0 || authorization.indexOf('.', second + 1) >= 0) throw new IllegalArgumentException();
            var decoder = Base64.getUrlDecoder();
            JsonNode header = json.readValue(decoder.decode(authorization.substring(7, first)), JsonNode.class);
            if (!"HS256".equals(header.get("alg").getStringValue())) throw new IllegalArgumentException();
            // doFinal resets the engine to its initialized key; engines are never shared across threads or verifiers.
            byte[] signature = hmac.get().doFinal(authorization.substring(7, second).getBytes(StandardCharsets.US_ASCII));
            if (!MessageDigest.isEqual(signature, decoder.decode(authorization.substring(second + 1)))) throw new IllegalArgumentException();
            payload = json.readValue(decoder.decode(authorization.substring(first + 1, second)), JsonNode.class);
            JsonNode exp = payload.get("exp");
            if (exp == null || !exp.isNumber() || exp.getNumberValue().doubleValue() <= System.currentTimeMillis() / 1000)
                throw new IllegalArgumentException();
        } catch (Exception e) {
            throw new ApiException(401, "invalid or expired token");
        }
        try {
            JsonNode sub = payload.get("sub"), name = payload.get("username");
            if (sub == null || !sub.isString() || name == null || !name.isString()) throw new IllegalArgumentException();
            String subject = sub.getStringValue();
            for (int i = 0; i < subject.length(); i++)
                if (subject.charAt(i) < '0' || subject.charAt(i) > '9') throw new IllegalArgumentException();
            long id = Long.parseLong(subject);
            if (id <= 0) throw new IllegalArgumentException();
            return new User(id, name.getStringValue());
        } catch (Exception e) {
            throw new ApiException(401, "invalid token payload");
        }
    }
}
