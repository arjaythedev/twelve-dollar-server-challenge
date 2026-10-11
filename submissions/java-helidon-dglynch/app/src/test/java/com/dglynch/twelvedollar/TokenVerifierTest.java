package com.dglynch.twelvedollar;

import module java.base;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class TokenVerifierTest {
    private static final String JWT_SECRET = "test-secret";

    private final TokenVerifier tokenVerifier = new TokenVerifier(JWT_SECRET);

    @Test
    void verifiesHs256TokenAndReadsIdentityClaims() throws Exception {
        var payload = """
                {"sub":"42","username":"Alice / Example","exp":%d}
                """.formatted(Instant.now().getEpochSecond() + 60);

        var token = token("HS256", payload, JWT_SECRET);
        var authenticatedUser = tokenVerifier.authenticatedUser(tokenVerifier.verifyToken(token));

        assertTrue(authenticatedUser.isPresent());
        assertEquals(42L, authenticatedUser.get().userId());
        assertEquals("Alice / Example", authenticatedUser.get().username());
    }

    @Test
    void rejectsBadSignatureExpiredAndNonHs256Tokens() throws Exception {
        var futureExpiration = Instant.now().getEpochSecond() + 60;
        var expired = Instant.now().minusMillis(1);
        var expiredExpiration = BigDecimal.valueOf(expired.getEpochSecond())
                .add(BigDecimal.valueOf(expired.getNano(), 9));

        assertThrows(RuntimeException.class, () ->
                tokenVerifier.verifyToken(token("HS256", payload(futureExpiration), "wrong-secret")));
        assertThrows(RuntimeException.class, () ->
                tokenVerifier.verifyToken(token("HS256",
                        "{\"sub\":\"42\",\"username\":\"Alice\",\"exp\":" + expiredExpiration.toPlainString() + "}",
                        JWT_SECRET)));
        assertThrows(RuntimeException.class, () ->
                tokenVerifier.verifyToken(token("HS384", payload(futureExpiration), JWT_SECRET)));
        assertThrows(RuntimeException.class, () -> tokenVerifier.verifyToken("not.a.jwt"));
    }

    @Test
    void rejectsInvalidIdentityClaimTypesAndValues() throws Exception {
        var expiration = Instant.now().getEpochSecond() + 60;

        for (var sub : new String[]{"\"0\"", "\"-1\"", "\"abc\"", "\"1.5\"", "42"}) {
            var token = token("HS256",
                    "{\"sub\":" + sub + ",\"username\":\"Alice\",\"exp\":" + expiration + "}", JWT_SECRET);
            assertTrue(tokenVerifier.authenticatedUser(tokenVerifier.verifyToken(token)).isEmpty());
        }

        var numericUsername = token("HS256",
                "{\"sub\":\"42\",\"username\":7,\"exp\":" + expiration + "}", JWT_SECRET);
        assertTrue(tokenVerifier.authenticatedUser(tokenVerifier.verifyToken(numericUsername)).isEmpty());
    }

    private String payload(long expiration) {
        return "{\"sub\":\"42\",\"username\":\"Alice\",\"exp\":" + expiration + "}";
    }

    private String token(String algorithm, String payload, String signingSecret) throws Exception {
        var encoder = Base64.getUrlEncoder().withoutPadding();
        var header = encoder.encodeToString(("{\"alg\":\"" + algorithm + "\"}").getBytes(StandardCharsets.UTF_8));
        var encodedPayload = encoder.encodeToString(payload.getBytes(StandardCharsets.UTF_8));
        var signingInput = header + "." + encodedPayload;
        var mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(signingSecret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        var signature = encoder.encodeToString(mac.doFinal(signingInput.getBytes(StandardCharsets.UTF_8)));
        return signingInput + "." + signature;
    }
}
