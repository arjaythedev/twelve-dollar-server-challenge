package server.util;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;

public final class FastJwt {

    public sealed interface AuthResult {
        record Success(long userId, String username) implements AuthResult {}
        record Failure(int statusCode, String errorMessage) implements AuthResult {}
    }

    private static final AuthResult.Failure MISSING_BEARER = new AuthResult.Failure(401, "missing bearer token");
    private static final AuthResult.Failure INVALID_TOKEN = new AuthResult.Failure(401, "invalid or expired token");
    private static final AuthResult.Failure INVALID_PAYLOAD = new AuthResult.Failure(401, "invalid token payload");

    private static final String STANDARD_HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

    private final SecretKeySpec secretKeySpec;
    private final ThreadLocal<Mac> threadLocalMac;
    private final ThreadLocal<byte[]> asciiScratch = ThreadLocal.withInitial(() -> new byte[512]);

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
            return MISSING_BEARER;
        }

        int tokenStart = 7;
        int len = authHeader.length();
        int dot1 = authHeader.indexOf('.', tokenStart);
        if (dot1 < 0) return INVALID_TOKEN;
        int dot2 = authHeader.indexOf('.', dot1 + 1);
        if (dot2 < 0 || authHeader.indexOf('.', dot2 + 1) >= 0) {
            return INVALID_TOKEN;
        }

        int signableLen = dot2 - tokenStart;
        byte[] scratch = asciiScratch.get();
        if (scratch.length < signableLen) {
            scratch = new byte[signableLen + 64];
            asciiScratch.set(scratch);
        }
        for (int i = 0; i < signableLen; i++) {
            scratch[i] = (byte) authHeader.charAt(tokenStart + i);
        }

        // Decode signature
        String sigB64 = authHeader.substring(dot2 + 1);
        byte[] sigBytes;
        try {
            sigBytes = Base64.getUrlDecoder().decode(sigB64);
        } catch (IllegalArgumentException e) {
            return INVALID_TOKEN;
        }

        // Verify HMAC-SHA256 signature
        try {
            Mac mac = threadLocalMac.get();
            mac.reset();
            mac.update(scratch, 0, signableLen);
            byte[] expectedSig = mac.doFinal();
            if (!MessageDigest.isEqual(expectedSig, sigBytes)) {
                return INVALID_TOKEN;
            }
        } catch (Exception e) {
            return INVALID_TOKEN;
        }

        // Verify header
        int headLen = dot1 - tokenStart;
        boolean standardHeader = (headLen == STANDARD_HEADER_B64.length()
                && authHeader.regionMatches(tokenStart, STANDARD_HEADER_B64, 0, headLen));

        if (!standardHeader) {
            String headB64 = authHeader.substring(tokenStart, dot1);
            byte[] headBytes;
            try {
                headBytes = Base64.getUrlDecoder().decode(headB64);
            } catch (IllegalArgumentException e) {
                return INVALID_TOKEN;
            }
            String headJson = new String(headBytes, StandardCharsets.UTF_8);
            String alg = extractStringField(headJson, "alg");
            if (!"HS256".equals(alg)) {
                return INVALID_TOKEN;
            }
        }

        // Verify payload
        String payloadB64 = authHeader.substring(dot1 + 1, dot2);
        byte[] payloadBytes;
        try {
            payloadBytes = Base64.getUrlDecoder().decode(payloadB64);
        } catch (IllegalArgumentException e) {
            return INVALID_TOKEN;
        }

        String payloadJson = new String(payloadBytes, StandardCharsets.UTF_8);
        long now = System.currentTimeMillis() / 1000L;

        Long exp = extractNumberField(payloadJson, "exp");
        if (exp != null && now >= exp) {
            return INVALID_TOKEN;
        }

        Long nbf = extractNumberField(payloadJson, "nbf");
        if (nbf != null && now < nbf) {
            return INVALID_TOKEN;
        }

        // SPEC: sub must be a positive integer STRING
        String subStr = extractStringField(payloadJson, "sub");
        if (subStr == null) {
            return INVALID_PAYLOAD;
        }
        long userId = StringUtil.parsePositiveId(subStr, 0, subStr.length());
        if (userId <= 0) {
            return INVALID_PAYLOAD;
        }

        // SPEC: username must be a string
        String username = extractStringField(payloadJson, "username");
        if (username == null) {
            return INVALID_PAYLOAD;
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
        return null; // Unclosed string
    }

    /**
     * Extracts a top-level numeric field: "key":123
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

        int valEnd = valStart;
        while (valEnd < json.length() && (Character.isDigit(json.charAt(valEnd)) || json.charAt(valEnd) == '-')) {
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
