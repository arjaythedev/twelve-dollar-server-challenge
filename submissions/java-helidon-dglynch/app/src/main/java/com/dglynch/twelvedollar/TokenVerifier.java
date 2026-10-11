package com.dglynch.twelvedollar;

import module java.base;
import io.helidon.json.JsonObject;
import io.helidon.json.JsonParser;
import io.helidon.json.JsonValueType;

public class TokenVerifier {
    private final SecretKeySpec secretKeySpec;

    public TokenVerifier(String jwtSecret) {
        this.secretKeySpec = new SecretKeySpec(jwtSecret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
    }

    public VerifiedToken verifyToken(String token) {
        try {
            var parts = token.split("\\.", -1);
            if (parts.length != 3 || !isBase64UrlSegment(parts[0]) || !isBase64UrlSegment(parts[1])
                    || !isBase64UrlSegment(parts[2])) {
                throw new InvalidJwtException();
            }

            var decoder = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT);
            var header = JsonParser.create(decoder.decode(ByteBuffer.wrap(Base64.getUrlDecoder().decode(parts[0]))).toString())
                    .readJsonObject();
            if (header.stringValue("alg").filter("HS256"::equals).isEmpty()) {
                throw new InvalidJwtException();
            }

            var signature = Base64.getUrlDecoder().decode(parts[2]);
            var mac = Mac.getInstance("HmacSHA256");
            mac.init(secretKeySpec);
            var expectedSignature = mac.doFinal((parts[0] + "." + parts[1]).getBytes(StandardCharsets.UTF_8));
            if (!MessageDigest.isEqual(expectedSignature, signature)) {
                throw new InvalidJwtException();
            }

            var payloadDecoder = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT);
            var payload = JsonParser.create(payloadDecoder.decode(ByteBuffer.wrap(Base64.getUrlDecoder().decode(parts[1]))).toString())
                    .readJsonObject();
            var expiration = payload.numberValue("exp").orElseThrow(InvalidJwtException::new);
            var now = Instant.now();
            var nowInSeconds = BigDecimal.valueOf(now.getEpochSecond())
                    .add(BigDecimal.valueOf(now.getNano(), 9));
            if (expiration.compareTo(nowInSeconds) <= 0) {
                throw new InvalidJwtException();
            }
            return new VerifiedToken(payload);
        } catch (InvalidJwtException e) {
            throw e;
        } catch (CharacterCodingException | RuntimeException e) {
            throw new InvalidJwtException();
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("Unable to verify JWT", e);
        }
    }

    public Optional<AuthenticatedUser> authenticatedUser(VerifiedToken verifiedToken) {
        var userId = positiveIntegerClaim(verifiedToken.payload(), "sub");
        var username = stringClaim(verifiedToken.payload(), "username");
        if (userId.isEmpty() || username.isEmpty()) {
            return Optional.empty();
        }
        return Optional.of(new AuthenticatedUser(userId.get(), username.get()));
    }

    private Optional<Long> positiveIntegerClaim(JsonObject payload, String claim) {
        var value = stringClaim(payload, claim);
        if (value.isEmpty() || !value.get().matches("[0-9]+")) {
            return Optional.empty();
        }
        try {
            var userId = Long.parseLong(value.get());
            return userId > 0 ? Optional.of(userId) : Optional.empty();
        } catch (NumberFormatException _) {
            return Optional.empty();
        }
    }

    private Optional<String> stringClaim(JsonObject payload, String claim) {
        return payload.value(claim)
                .filter(value -> value.type() == JsonValueType.STRING)
                .map(value -> value.asString().value());
    }

    private static boolean isBase64UrlSegment(String segment) {
        return !segment.isEmpty() && segment.chars().allMatch(character ->
                character >= 'A' && character <= 'Z'
                        || character >= 'a' && character <= 'z'
                        || character >= '0' && character <= '9'
                        || character == '-' || character == '_');
    }

    public record VerifiedToken(JsonObject payload) {
    }

    public record AuthenticatedUser(long userId, String username) {
    }

    public static final class InvalidJwtException extends RuntimeException {
        @Serial
        private static final long serialVersionUID = 1L;
    }
}
