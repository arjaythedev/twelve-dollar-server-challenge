package server.util;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Base64;

public final class FastJwt {

    public sealed interface AuthResult {
        record Success(long userId, String username) implements AuthResult {}
        record Failure(int statusCode, String errorMessage) implements AuthResult {}
    }

    private final SecretKeySpec secretKeySpec;
    private final ThreadLocal<Mac> threadLocalMac;

    public FastJwt(String secret) {
        byte[] secretBytes = secret.getBytes(StandardCharsets.UTF_8);
        this.secretKeySpec = new SecretKeySpec(secretBytes, "HmacSHA256");
        this.threadLocalMac = ThreadLocal.withInitial(() -> {
            try {
                Mac mac = Mac.getInstance("HmacSHA256");
                mac.init(secretKeySpec);
                return mac;
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        });
    }

    public AuthResult authenticate(String authHeader) {
        if (authHeader == null || !authHeader.startsWith("Bearer ")) {
            return new AuthResult.Failure(401, "missing bearer token");
        }

        String token = authHeader.substring(7);
        int dot1 = token.indexOf('.');
        if (dot1 < 0) return new AuthResult.Failure(401, "invalid or expired token");
        int dot2 = token.indexOf('.', dot1 + 1);
        if (dot2 < 0 || token.indexOf('.', dot2 + 1) >= 0) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        String headB64 = token.substring(0, dot1);
        String payloadB64 = token.substring(dot1 + 1, dot2);
        String sigB64 = token.substring(dot2 + 1);

        byte[] headBytes;
        byte[] payloadBytes;
        byte[] sigBytes;
        try {
            headBytes = Base64.getUrlDecoder().decode(headB64);
            payloadBytes = Base64.getUrlDecoder().decode(payloadB64);
            sigBytes = Base64.getUrlDecoder().decode(sigB64);
        } catch (IllegalArgumentException e) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        // Verify HMAC-SHA256 signature
        try {
            Mac mac = threadLocalMac.get();
            mac.reset();
            mac.update(token.substring(0, dot2).getBytes(StandardCharsets.US_ASCII));
            byte[] expectedSig = mac.doFinal();
            if (!MessageDigest.isEqual(expectedSig, sigBytes)) {
                return new AuthResult.Failure(401, "invalid or expired token");
            }
        } catch (Exception e) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        // Verify header
        String headJson = new String(headBytes, StandardCharsets.UTF_8);
        String alg = extractStringField(headJson, "alg");
        if (!"HS256".equals(alg)) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        // Verify payload
        String payloadJson = new String(payloadBytes, StandardCharsets.UTF_8);
        long now = System.currentTimeMillis() / 1000L;

        Long exp = extractNumberField(payloadJson, "exp");
        if (exp != null && now >= exp) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        Long nbf = extractNumberField(payloadJson, "nbf");
        if (nbf != null && now < nbf) {
            return new AuthResult.Failure(401, "invalid or expired token");
        }

        // SPEC: sub must be a positive integer STRING
        String subStr = extractStringField(payloadJson, "sub");
        if (subStr == null) {
            return new AuthResult.Failure(401, "invalid token payload");
        }
        Long userId = StringUtil.parsePositiveId(subStr);
        if (userId == null) {
            return new AuthResult.Failure(401, "invalid token payload");
        }

        // SPEC: username must be a string
        String username = extractStringField(payloadJson, "username");
        if (username == null) {
            return new AuthResult.Failure(401, "invalid token payload");
        }

        return new AuthResult.Success(userId, username);
    }

    /**
     * Extracts a top-level string field from simple JSON: "key":"value"
     * Returns null if key is not found or value is not a string.
     */
    static String extractStringField(String json, String key) {
        String pattern = "\"" + key + "\"";
        int idx = json.indexOf(pattern);
        if (idx < 0) return null;
        int colon = json.indexOf(':', idx + pattern.length());
        if (colon < 0) return null;

        // Skip whitespace after colon
        int valStart = colon + 1;
        while (valStart < json.length() && Character.isWhitespace(json.charAt(valStart))) {
            valStart++;
        }
        if (valStart >= json.length() || json.charAt(valStart) != '"') {
            return null; // Not a string!
        }
        valStart++; // skip opening quote

        StringBuilder sb = new StringBuilder();
        boolean escaped = false;
        for (int i = valStart; i < json.length(); i++) {
            char c = json.charAt(i);
            if (escaped) {
                switch (c) {
                    case '"' -> sb.append('"');
                    case '\\' -> sb.append('\\');
                    case '/' -> sb.append('/');
                    case 'b' -> sb.append('\b');
                    case 'f' -> sb.append('\f');
                    case 'n' -> sb.append('\n');
                    case 'r' -> sb.append('\r');
                    case 't' -> sb.append('\t');
                    case 'u' -> {
                        if (i + 4 < json.length()) {
                            try {
                                int code = Integer.parseInt(json.substring(i + 1, i + 5), 16);
                                sb.append((char) code);
                                i += 4;
                            } catch (NumberFormatException e) {
                                sb.append('u');
                            }
                        } else {
                            sb.append('u');
                        }
                    }
                    default -> sb.append(c);
                }
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                return sb.toString();
            } else {
                sb.append(c);
            }
        }
        return null; // unclosed string
    }

    /**
     * Extracts a top-level numeric field: "key": 123456
     */
    static Long extractNumberField(String json, String key) {
        String pattern = "\"" + key + "\"";
        int idx = json.indexOf(pattern);
        if (idx < 0) return null;
        int colon = json.indexOf(':', idx + pattern.length());
        if (colon < 0) return null;

        int valStart = colon + 1;
        while (valStart < json.length() && Character.isWhitespace(json.charAt(valStart))) {
            valStart++;
        }
        if (valStart >= json.length()) return null;
        if (json.charAt(valStart) == '"') return null; // It's a string, not a number

        int valEnd = valStart;
        if (json.charAt(valEnd) == '-') valEnd++;
        while (valEnd < json.length() && Character.isDigit(json.charAt(valEnd))) {
            valEnd++;
        }
        if (valEnd == valStart) return null;

        try {
            return Long.parseLong(json.substring(valStart, valEnd));
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
