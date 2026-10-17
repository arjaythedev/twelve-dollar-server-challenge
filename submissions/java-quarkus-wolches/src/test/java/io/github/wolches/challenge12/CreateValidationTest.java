package io.github.wolches.challenge12;

import io.github.wolches.challenge12.support.ApiTestBase;
import io.github.wolches.challenge12.support.Jwt;
import io.github.wolches.challenge12.support.TestDatabaseResource;
import io.quarkus.test.common.QuarkusTestResource;
import io.quarkus.test.junit.QuarkusTest;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;

/**
 * {@code -- create validation}: request bodies that {@code POST /posts} must reject.
 */
@QuarkusTest
@QuarkusTestResource(TestDatabaseResource.class)
@Order(4)
class CreateValidationTest extends ApiTestBase {

    private static final String AUTH = Jwt.bearer(Jwt.validToken());

    private static final String REQUIRED = "{\"error\":\"body is required\"}";

    @Test
    void emptyBody() {
        expect(post("/posts", AUTH, "{\"body\":\"\"}"), 400, REQUIRED);
    }

    @Test
    void whitespaceBody() {
        expect(post("/posts", AUTH, "{\"body\":\"   \"}"), 400, REQUIRED);
    }

    @Test
    void numericBody() {
        expect(post("/posts", AUTH, "{\"body\":123}"), 400, REQUIRED);
    }

    @Test
    void missingBody() {
        expect(post("/posts", AUTH, "{}"), 400, REQUIRED);
    }

    @Test
    void malformedJson() {
        expect(post("/posts", AUTH, "{bad json"), 400, "{\"error\":\"malformed JSON body\"}");
    }

    @Test
    void bodyOf501Characters() {
        expect(post("/posts", AUTH, "{\"body\":\"" + "a".repeat(501) + "\"}"), 400,
                "{\"error\":\"body must be at most 500 characters\"}");
    }
}
