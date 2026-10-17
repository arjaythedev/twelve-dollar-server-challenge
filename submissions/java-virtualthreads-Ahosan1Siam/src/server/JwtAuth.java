package server;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;

public final class JwtAuth {
    private static final Base64.Decoder B64_URL_DEC = Base64.getUrlDecoder();
    private static volatile byte[] secretBytes;

    private static final ThreadLocal<Mac> THREAD_LOCAL_MAC = ThreadLocal.withInitial(() -> {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secretBytes, "HmacSHA256"));
            return mac;
        } catch (Exception e) {
            throw new RuntimeException("Failed to initialize HMAC-SHA256", e);
        }
    });

    public static void init(String secret) {
        secretBytes = secret.getBytes(StandardCharsets.UTF_8);
    }

    public static class AuthResult {
        public long userId;
        public String username;
        public int status = 200;
        public String errorJson;
    }

    public static AuthResult authenticate(String authHeader) {
        AuthResult res = new AuthResult();
        if (authHeader == null || !authHeader.startsWith("Bearer ")) {
            res.status = 401;
            res.errorJson = "{\"error\":\"missing bearer token\"}";
            return res;
        }

        String token = authHeader.substring(7).trim();
        int dot1 = token.indexOf('.');
        int dot2 = (dot1 == -1) ? -1 : token.indexOf('.', dot1 + 1);
        if (dot1 == -1 || dot2 == -1 || token.indexOf('.', dot2 + 1) != -1) {
            res.status = 401;
            res.errorJson = "{\"error\":\"invalid or expired token\"}";
            return res;
        }

        String headerB64 = token.substring(0, dot1);
        String payloadB64 = token.substring(dot1 + 1, dot2);
        String sigB64 = token.substring(dot2 + 1);

        try {
            Mac mac = THREAD_LOCAL_MAC.get();
            mac.reset();
            mac.update(token.substring(0, dot2).getBytes(StandardCharsets.US_ASCII));
            byte[] computedSig = mac.doFinal();
            byte[] expectedSig = B64_URL_DEC.decode(sigB64);

            if (!MessageDigest.isEqual(computedSig, expectedSig)) {
                res.status = 401;
                res.errorJson = "{\"error\":\"invalid or expired token\"}";
                return res;
            }

            // Decode and inspect header
            String headerJson = new String(B64_URL_DEC.decode(headerB64), StandardCharsets.UTF_8);
            if (!headerJson.contains("\"alg\":\"HS256\"") && !headerJson.contains("\"alg\": \"HS256\"")) {
                res.status = 401;
                res.errorJson = "{\"error\":\"invalid or expired token\"}";
                return res;
            }

            // Decode and inspect payload
            String payloadJson = new String(B64_URL_DEC.decode(payloadB64), StandardCharsets.UTF_8);

            // Check expiration
            long nowSec = System.currentTimeMillis() / 1000;
            Long exp = parseLongField(payloadJson, "exp");
            if (exp != null && exp < nowSec) {
                res.status = 401;
                res.errorJson = "{\"error\":\"invalid or expired token\"}";
                return res;
            }

            // Check sub (must be an integer)
            Long sub = parseLongField(payloadJson, "sub");
            if (sub == null) {
                res.status = 401;
                res.errorJson = "{\"error\":\"invalid token payload\"}";
                return res;
            }

            // Check username (must be a non-empty string)
            String username = parseStringField(payloadJson, "username");
            if (username == null || username.isEmpty()) {
                res.status = 401;
                res.errorJson = "{\"error\":\"invalid token payload\"}";
                return res;
            }

            res.userId = sub;
            res.username = username;
            res.status = 200;
            return res;
        } catch (Exception e) {
            res.status = 401;
            res.errorJson = "{\"error\":\"invalid or expired token\"}";
            return res;
        }
    }

    private static Long parseLongField(String json, String field) {
        String key = "\"" + field + "\"";
        int idx = json.indexOf(key);
        if (idx == -1) return null;
        int colon = json.indexOf(':', idx + key.length());
        if (colon == -1) return null;
        int start = colon + 1;
        while (start < json.length() && Character.isWhitespace(json.charAt(start))) start++;
        if (start < json.length() && json.charAt(start) == '"') {
            int end = json.indexOf('"', start + 1);
            if (end == -1) return null;
            try {
                return Long.parseLong(json.substring(start + 1, end));
            } catch (NumberFormatException e) {
                return null;
            }
        }
        int end = start;
        while (end < json.length() && (Character.isDigit(json.charAt(end)) || json.charAt(end) == '-')) end++;
        if (start == end) return null;
        try {
            return Long.parseLong(json.substring(start, end));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static String parseStringField(String json, String field) {
        String key = "\"" + field + "\"";
        int idx = json.indexOf(key);
        if (idx == -1) return null;
        int colon = json.indexOf(':', idx + key.length());
        if (colon == -1) return null;
        int start = json.indexOf('"', colon + 1);
        if (start == -1) return null;
        int end = json.indexOf('"', start + 1);
        if (end == -1) return null;
        return json.substring(start + 1, end);
    }
}

