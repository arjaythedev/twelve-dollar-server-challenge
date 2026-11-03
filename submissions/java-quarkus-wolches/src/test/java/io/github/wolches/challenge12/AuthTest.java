package io.github.wolches.challenge12;

import io.github.wolches.challenge12.support.ApiTestBase;
import io.github.wolches.challenge12.support.Jwt;
import io.github.wolches.challenge12.support.TestDatabaseResource;
import io.quarkus.test.common.QuarkusTestResource;
import io.quarkus.test.junit.QuarkusTest;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;

/**
 * {@code -- auth}: every way a token can be missing or wrong on {@code POST /posts}.
 */
@QuarkusTest
@QuarkusTestResource(TestDatabaseResource.class)
@Order(3)
class AuthTest extends ApiTestBase {

    private static final String TOKEN = Jwt.validToken();
    private static final String BODY = "{\"body\":\"x\"}";

    private static final String MISSING = "{\"error\":\"missing bearer token\"}";
    private static final String INVALID = "{\"error\":\"invalid or expired token\"}";
    private static final String PAYLOAD = "{\"error\":\"invalid token payload\"}";

    @Test
    void noToken() {
        expect(post("/posts", NO_AUTH, BODY), 401, MISSING);
    }

    @Test
    void basicAuth() {
        expect(post("/posts", "Basic abc", BODY), 401, MISSING);
    }

    @Test
    void garbageToken() {
        expect(post("/posts", "Bearer abc", BODY), 401, INVALID);
    }

    @Test
    void badSignature() {
        String unsigned = TOKEN.substring(0, TOKEN.lastIndexOf('.'));
        String token = unsigned + ".c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ";
        expect(post("/posts", Jwt.bearer(token), BODY), 401, INVALID);
    }

    @Test
    void algNone() {
        expect(post("/posts", Jwt.bearer(Jwt.noneAlg(TOKEN)), BODY), 401, INVALID);
    }

    @Test
    void expiredToken() {
        long now = Jwt.now();
        expect(post("/posts", Jwt.bearer(Jwt.token(now - 7200, now - 3600)), BODY), 401, INVALID);
    }

    @Test
    void subNotAnInteger() {
        long now = Jwt.now();
        String token = Jwt.sign("{\"sub\":\"abc\",\"username\":\"" + Jwt.USER_NAME + "\",\"iat\":" + now
                + ",\"exp\":" + (now + 3600) + "}");
        expect(post("/posts", Jwt.bearer(token), BODY), 401, PAYLOAD);
    }

    @Test
    void noUsername() {
        long now = Jwt.now();
        String token = Jwt.sign("{\"sub\":\"" + Jwt.USER_ID + "\",\"iat\":" + now + ",\"exp\":" + (now + 3600) + "}");
        expect(post("/posts", Jwt.bearer(token), BODY), 401, PAYLOAD);
    }

    /** Auth is checked before the id: a bad id without a token is still 401. */
    @Test
    void likeWithoutTokenChecksAuthBeforeId() {
        expect(post("/posts/abc/like", NO_AUTH, NO_BODY), 401, MISSING);
    }
}
