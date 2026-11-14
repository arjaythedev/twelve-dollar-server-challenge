package server.util;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

import static org.junit.jupiter.api.Assertions.*;

public class FastJwtTest {

    private static final String SECRET = "twelve-dollar-challenge";
    private FastJwt jwt;

    @BeforeEach
    void setup() {
        jwt = new FastJwt(SECRET);
    }

    private String sign(String payload) throws Exception {
        String head = Base64.getUrlEncoder().withoutPadding().encodeToString("{\"alg\":\"HS256\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.UTF_8));
        String body = Base64.getUrlEncoder().withoutPadding().encodeToString(payload.getBytes(StandardCharsets.UTF_8));
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(SECRET.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        byte[] sig = mac.doFinal((head + "." + body).getBytes(StandardCharsets.US_ASCII));
        String sigB64 = Base64.getUrlEncoder().withoutPadding().encodeToString(sig);
        return head + "." + body + "." + sigB64;
    }

    @Test
    void testValidToken() throws Exception {
        long now = System.currentTimeMillis() / 1000L;
        String token = sign("{\"sub\":\"123\",\"username\":\"testuser\",\"iat\":" + now + ",\"exp\":" + (now + 3600) + "}");
        FastJwt.AuthResult result = jwt.authenticate("Bearer " + token);

        assertInstanceOf(FastJwt.AuthResult.Success.class, result);
        FastJwt.AuthResult.Success success = (FastJwt.AuthResult.Success) result;
        assertEquals(123L, success.userId());
        assertEquals("testuser", success.username());
    }

    @Test
    void testMissingOrInvalidHeader() {
        FastJwt.AuthResult res1 = jwt.authenticate(null);
        assertInstanceOf(FastJwt.AuthResult.Failure.class, res1);
        assertEquals("missing bearer token", ((FastJwt.AuthResult.Failure) res1).errorMessage());

        FastJwt.AuthResult res2 = jwt.authenticate("Basic abc");
        assertInstanceOf(FastJwt.AuthResult.Failure.class, res2);
        assertEquals("missing bearer token", ((FastJwt.AuthResult.Failure) res2).errorMessage());
    }

    @Test
    void testGarbageToken() {
        FastJwt.AuthResult res = jwt.authenticate("Bearer abc");
        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid or expired token", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }

    @Test
    void testTamperedSignature() throws Exception {
        long now = System.currentTimeMillis() / 1000L;
        String token = sign("{\"sub\":\"123\",\"username\":\"testuser\",\"iat\":" + now + ",\"exp\":" + (now + 3600) + "}");
        String tampered = token.substring(0, token.lastIndexOf('.') + 1) + "c2lnbmF0dXJlLWlzLW5vdC12YWxpZA";
        FastJwt.AuthResult res = jwt.authenticate("Bearer " + tampered);

        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid or expired token", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }

    @Test
    void testAlgNone() {
        String head = Base64.getUrlEncoder().withoutPadding().encodeToString("{\"alg\":\"none\",\"typ\":\"JWT\"}".getBytes(StandardCharsets.UTF_8));
        String body = Base64.getUrlEncoder().withoutPadding().encodeToString("{\"sub\":\"1\",\"username\":\"u\"}".getBytes(StandardCharsets.UTF_8));
        String token = head + "." + body + ".";
        FastJwt.AuthResult res = jwt.authenticate("Bearer " + token);

        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid or expired token", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }

    @Test
    void testExpiredToken() throws Exception {
        long now = System.currentTimeMillis() / 1000L;
        String token = sign("{\"sub\":\"123\",\"username\":\"testuser\",\"iat\":" + (now - 7200) + ",\"exp\":" + (now - 3600) + "}");
        FastJwt.AuthResult res = jwt.authenticate("Bearer " + token);

        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid or expired token", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }

    @Test
    void testSubNotInteger() throws Exception {
        long now = System.currentTimeMillis() / 1000L;
        String token = sign("{\"sub\":\"abc\",\"username\":\"testuser\",\"iat\":" + now + ",\"exp\":" + (now + 3600) + "}");
        FastJwt.AuthResult res = jwt.authenticate("Bearer " + token);

        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid token payload", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }

    @Test
    void testMissingUsername() throws Exception {
        long now = System.currentTimeMillis() / 1000L;
        String token = sign("{\"sub\":\"123\",\"iat\":" + now + ",\"exp\":" + (now + 3600) + "}");
        FastJwt.AuthResult res = jwt.authenticate("Bearer " + token);

        assertInstanceOf(FastJwt.AuthResult.Failure.class, res);
        assertEquals("invalid token payload", ((FastJwt.AuthResult.Failure) res).errorMessage());
    }
}
