package io.github.wolches.challenge12;

import io.github.wolches.challenge12.support.ApiTestBase;
import io.github.wolches.challenge12.support.TestDatabaseResource;
import io.quarkus.test.common.QuarkusTestResource;
import io.quarkus.test.junit.QuarkusTest;
import io.restassured.response.Response;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * {@code -- health, 404s, id validation}.
 */
@QuarkusTest
@QuarkusTestResource(TestDatabaseResource.class)
@Order(2)
class HealthAndNotFoundTest extends ApiTestBase {

    @Test
    void health() {
        Response response = get("/health");
        String body = body(response);
        assertTrue(response.statusCode() == 200
                        && body.matches("\\{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":\\d+\\}"),
                "GET /health: got " + response.statusCode() + " " + body);
    }

    @Test
    void unknownPath() {
        expect(get("/nope"), 404, "{\"error\":\"not found\"}");
    }

    @ParameterizedTest(name = "GET /posts/{0}")
    @ValueSource(strings = {"0", "-1", "abc", "1.5"})
    void invalidPostId(String id) {
        expect(get("/posts/" + id), 400, "{\"error\":\"invalid post id\"}");
    }

    @Test
    void missingPost() {
        expect(get("/posts/999999999"), 404, "{\"error\":\"post not found\"}");
    }
}
