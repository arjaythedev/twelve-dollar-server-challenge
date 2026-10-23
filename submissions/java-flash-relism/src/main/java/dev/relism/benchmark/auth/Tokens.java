package dev.relism.benchmark.auth;

import dev.relism.fpr.core.ByteView;
import dev.relism.benchmark.http.Json;
import dev.relism.flash.http.HttpException;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Owned by the SQLite executor; only the HMAC key state is reused, never verification results. */
public final class Tokens {
    public record Proof(byte[] header, byte[] payload) {}
    private final Mac mac;

    public Tokens(String secret) {
        try {
            mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        } catch (GeneralSecurityException e) { throw new IllegalStateException(e); }
    }

    public Proof verify(ByteView authorization) {
        if (authorization == null || authorization.length() < 7)
            throw new HttpException(401, "missing bearer token");
        String prefix = "Bearer ";
        for (int i = 0; i < prefix.length(); i++) {
            if (authorization.byteAt(i) != prefix.charAt(i))
                throw new HttpException(401, "missing bearer token");
        }
        try {
            byte[] token = new byte[authorization.length() - 7];
            int first = -1, second = -1;
            for (int i = 0; i < token.length; i++) {
                int c = authorization.byteAt(i + 7) & 0xff;
                if (c > 127) throw new IllegalArgumentException();
                token[i] = (byte) c;
                if (c == '.') {
                    if (first < 0) first = i;
                    else if (second < 0) second = i;
                    else throw new IllegalArgumentException();
                }
            }
            if (first <= 0 || second <= first + 1) throw new IllegalArgumentException();
            var decoder = Base64.getUrlDecoder();
            byte[] header = decoder.decode(ByteBuffer.wrap(token, 0, first)).array();
            byte[] payload = decoder.decode(ByteBuffer.wrap(token, first + 1, second - first - 1)).array();
            byte[] signature = decoder.decode(ByteBuffer.wrap(token, second + 1, token.length - second - 1)).array();
            mac.update(token, 0, second);
            byte[] expected = mac.doFinal();
            if (signature.length != 32 || !MessageDigest.isEqual(signature, expected)
                    || Json.utf8Length(header) < 0 || Json.utf8Length(payload) < 0)
                throw new IllegalArgumentException();
            return new Proof(header, payload);
        } catch (IllegalArgumentException e) {
            throw new HttpException(401, "invalid or expired token");
        } finally { mac.reset(); }
    }
}
