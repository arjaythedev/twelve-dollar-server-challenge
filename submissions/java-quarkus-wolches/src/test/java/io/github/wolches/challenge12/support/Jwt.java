package io.github.wolches.challenge12.support;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/**
 * HS256 JWT signing, a port of {@code sign()} from test/test.sh.
 */
public final class Jwt {

    public static final String SECRET = System.getenv().getOrDefault("JWT_SECRET", "twelve-dollar-challenge");
    /** User 1 in the seed. */
    public static final int USER_ID = 1;
    public static final String USER_NAME = "golden_ember_1";

    private static final String HEADER = b64url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.UTF_8));

    private Jwt() {
    }

    public static long now() {
        return System.currentTimeMillis() / 1000;
    }

    /** A token for user 1 that is valid for an hour. */
    public static String validToken() {
        long now = now();
        return token(now, now + 3600);
    }

    public static String token(long iat, long exp) {
        return sign("{\"sub\":\"" + USER_ID + "\",\"username\":\"" + USER_NAME + "\",\"iat\":" + iat + ",\"exp\":" + exp + "}");
    }

    public static String bearer(String token) {
        return "Bearer " + token;
    }

    /** {@code alg=none} variant of a signed token: the original payload with an empty signature. */
    public static String noneAlg(String token) {
        String payload = token.split("\\.")[1];
        return b64url("{\"alg\":\"none\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.UTF_8)) + "." + payload + ".";
    }

    public static String sign(String payloadJson) {
        String payload = b64url(payloadJson.getBytes(StandardCharsets.UTF_8));
        String signingInput = HEADER + "." + payload;
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(SECRET.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
            return signingInput + "." + b64url(mac.doFinal(signingInput.getBytes(StandardCharsets.UTF_8)));
        } catch (Exception e) {
            throw new IllegalStateException("cannot sign JWT", e);
        }
    }

    private static String b64url(byte[] bytes) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
    }
}
