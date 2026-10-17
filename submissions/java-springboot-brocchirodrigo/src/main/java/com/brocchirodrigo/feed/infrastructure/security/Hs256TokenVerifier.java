package com.brocchirodrigo.feed.infrastructure.security;

import com.brocchirodrigo.feed.application.gateway.TokenClaims;
import com.brocchirodrigo.feed.application.gateway.TokenVerifier;
import tools.jackson.core.JacksonException;
import tools.jackson.databind.JsonNode;
import tools.jackson.databind.json.JsonMapper;
import tools.jackson.databind.node.ObjectNode;
import tools.jackson.databind.node.StringNode;

import javax.crypto.Mac;
import javax.crypto.ShortBufferException;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.Base64;
import java.util.Optional;
import java.util.function.DoublePredicate;

public final class Hs256TokenVerifier implements TokenVerifier {

    private static final String ALGORITHM = "HmacSHA256";
    private static final int DIGEST_LENGTH = 32;
    private static final int SIGNATURE_LENGTH = 43;
    private static final int INITIAL_INPUT_BUFFER = 512;
    private static final Base64.Encoder ENCODER = Base64.getUrlEncoder().withoutPadding();
    private static final Base64.Decoder DECODER = Base64.getUrlDecoder();
    private static final String CANONICAL_HEADER =
            ENCODER.encodeToString("{\"alg\":\"HS256\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.US_ASCII));

    private final Object lock = new Object();
    private final Mac mac;
    private final JsonMapper json;
    private final byte[] digest = new byte[DIGEST_LENGTH];
    private final byte[] signature = new byte[SIGNATURE_LENGTH];
    private byte[] input = new byte[INITIAL_INPUT_BUFFER];

    public Hs256TokenVerifier(String secret, JsonMapper json) {
        this.mac = keyedMac(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), ALGORITHM));
        this.json = json;
    }

    @Override
    public Optional<TokenClaims> verify(String token) {
        var headerEnd = token.indexOf('.');
        var payloadEnd = token.indexOf('.', headerEnd + 1);
        if (headerEnd < 1 || payloadEnd < 0 || token.indexOf('.', payloadEnd + 1) >= 0) {
            return Optional.empty();
        }
        if (!isHs256(token, headerEnd) || !signatureMatches(token, payloadEnd)) {
            return Optional.empty();
        }
        return switch (decode(token.substring(headerEnd + 1, payloadEnd))) {
            case ObjectNode payload when isCurrent(payload) ->
                    Optional.of(new TokenClaims(text(payload.get("sub")), text(payload.get("username"))));
            case null, default -> Optional.empty();
        };
    }

    private boolean isHs256(String token, int headerEnd) {
        if (headerEnd == CANONICAL_HEADER.length() && token.startsWith(CANONICAL_HEADER)) {
            return true;
        }
        return decode(token.substring(0, headerEnd)) instanceof ObjectNode header
                && header.get("alg") instanceof StringNode algorithm
                && "HS256".equals(algorithm.stringValue());
    }

    private boolean signatureMatches(String token, int payloadEnd) {
        var length = token.length();
        if (length - payloadEnd - 1 != SIGNATURE_LENGTH) {
            return false;
        }
        synchronized (lock) {
            if (!copyAscii(token, length)) {
                return false;
            }
            sign(payloadEnd);
            var difference = 0;
            for (var i = 0; i < SIGNATURE_LENGTH; i++) {
                difference |= signature[i] ^ input[payloadEnd + 1 + i];
            }
            return difference == 0;
        }
    }

    private boolean copyAscii(String token, int length) {
        if (input.length < length) {
            input = new byte[Math.max(length, input.length * 2)];
        }
        for (var i = 0; i < length; i++) {
            var character = token.charAt(i);
            if (character > 127) {
                return false;
            }
            input[i] = (byte) character;
        }
        return true;
    }

    private void sign(int signingInputLength) {
        try {
            mac.update(input, 0, signingInputLength);
            mac.doFinal(digest, 0);
        } catch (ShortBufferException error) {
            throw new IllegalStateException(error);
        }
        ENCODER.encode(digest, signature);
    }

    private JsonNode decode(String segment) {
        try {
            return json.readTree(DECODER.decode(segment));
        } catch (IllegalArgumentException | JacksonException _) {
            return null;
        }
    }

    private static boolean isCurrent(ObjectNode payload) {
        var now = System.currentTimeMillis() / 1000.0;
        return within(payload.get("exp"), expiresAt -> now < expiresAt)
                && within(payload.get("nbf"), notBefore -> now >= notBefore);
    }

    private static boolean within(JsonNode claim, DoublePredicate condition) {
        return claim == null || claim.isNumber() && condition.test(claim.doubleValue());
    }

    private static String text(JsonNode claim) {
        return claim instanceof StringNode value ? value.stringValue() : null;
    }

    private static Mac keyedMac(SecretKeySpec key) {
        try {
            var keyed = Mac.getInstance(ALGORITHM);
            keyed.init(key);
            return keyed;
        } catch (GeneralSecurityException error) {
            throw new IllegalStateException(error);
        }
    }
}
