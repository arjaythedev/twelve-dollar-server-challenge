package io.github.wolches.challenge12;

import com.fasterxml.jackson.databind.JsonNode;
import io.github.wolches.challenge12.support.ApiTestBase;
import io.github.wolches.challenge12.support.Jwt;
import io.github.wolches.challenge12.support.TestDatabaseResource;
import io.quarkus.test.common.QuarkusTestResource;
import io.quarkus.test.junit.QuarkusTest;
import io.restassured.response.Response;
import org.junit.jupiter.api.MethodOrderer;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.TestMethodOrder;

import java.time.Duration;
import java.time.Instant;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * {@code -- create, like, read back}: the write path. The steps build on each other, so they run
 * in order and share the created post's id and timestamp.
 */
@QuarkusTest
@QuarkusTestResource(TestDatabaseResource.class)
@Order(5)
@TestMethodOrder(MethodOrderer.OrderAnnotation.class)
class CreateLikeReadTest extends ApiTestBase {

    private static final String AUTH = Jwt.bearer(Jwt.validToken());
    private static final String BODY500 = "b".repeat(500);

    private static Integer newId;
    private static String createdAt;
    private static JsonNode feed;

    @Test
    @Order(1)
    void specialCharactersRoundTripTrimmed() {
        Response response = post("/posts", AUTH,
                "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / café ✓  \"}");
        assertEquals(201, response.statusCode(), "got " + response.statusCode() + " " + body(response));
        assertEquals("test: <b>&amp;</b> \"quoted\" / café ✓",
                json(response).path("post").path("body").asText(), "got " + body(response));
    }

    @Test
    @Order(2)
    void create500CharPost() {
        Response response = post("/posts", AUTH, "{\"body\":\"  " + BODY500 + " \"}");
        JsonNode post = json(response).path("post");
        newId = post.path("id").isInt() ? post.path("id").asInt() : null;
        createdAt = post.path("created_at").isTextual() ? post.path("created_at").asText() : null;
        expect(response, 201, expectedPost(0));
        assertJsonContentType(response);
    }

    @Test
    @Order(3)
    void createdAtFormat() {
        requireCreatedPost();
        assertTrue(createdAt.matches("\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z"),
                "created_at format: got '" + createdAt + "'");
    }

    @Test
    @Order(4)
    void createdAtIsUtcNow() {
        requireCreatedPost();
        long drift = Math.abs(Duration.between(Instant.parse(createdAt), Instant.now()).getSeconds());
        assertTrue(drift <= 60, "created_at is not UTC now: drift " + drift + "s");
    }

    @Test
    @Order(5)
    void likeFirst() {
        requireCreatedPost();
        expect(post("/posts/" + newId + "/like", AUTH, NO_BODY), 201,
                "{\"liked\":true,\"already_liked\":false,\"post_id\":" + newId + "}");
    }

    @Test
    @Order(6)
    void likeRepeat() {
        requireCreatedPost();
        expect(post("/posts/" + newId + "/like", AUTH, NO_BODY), 200,
                "{\"liked\":true,\"already_liked\":true,\"post_id\":" + newId + "}");
    }

    @Test
    @Order(7)
    void likeMissingPost() {
        expect(post("/posts/999999999/like", AUTH, NO_BODY), 404, "{\"error\":\"post not found\"}");
    }

    @Test
    @Order(8)
    void likeBadIdWithToken() {
        expect(post("/posts/abc/like", AUTH, NO_BODY), 400, "{\"error\":\"invalid post id\"}");
    }

    @Test
    @Order(9)
    void readBackWithLikeCountOne() {
        requireCreatedPost();
        expect(get("/posts/" + newId), 200, expectedPost(1));
    }

    /** like_count reflects the new like at once, on a seeded post as well. */
    @Test
    @Order(10)
    void likeOnSeededPostIsCountedAtOnce() {
        int before = goldenJson("post-1.json").path("post").path("like_count").asInt();
        post("/posts/1/like", AUTH, NO_BODY);
        Response response = get("/posts/1");
        assertEquals(before + 1, json(response).path("post").path("like_count").asInt(-1),
                "seeded post like_count: got " + body(response));
    }

    @Test
    @Order(11)
    void feedHas20Posts() {
        assertEquals(20, feed().path("posts").size(), "feed length: " + feed());
    }

    @Test
    @Order(12)
    void newPostIsAtTheTopOfTheFeed() {
        requireCreatedPost();
        assertEquals(newId, feed().path("posts").path(0).path("id").asInt(-1), "top of feed: " + feed().path("posts").path(0));
    }

    @Test
    @Order(13)
    void feedEntryMatchesThePost() {
        requireCreatedPost();
        assertEquals(postJson(1), feed().path("posts").path(0).toString(), "feed entry");
    }

    private static JsonNode feed() {
        if (feed == null) {
            feed = json(get("/feed"));
        }
        return feed;
    }

    private static void requireCreatedPost() {
        assertNotNull(newId, "depends on create500CharPost, which did not return a post id");
        assertNotNull(createdAt, "depends on create500CharPost, which did not return created_at");
    }

    private static String postJson(int likeCount) {
        return "{\"id\":" + newId + ",\"body\":\"" + BODY500 + "\",\"created_at\":\"" + createdAt
                + "\",\"author\":\"" + Jwt.USER_NAME + "\",\"like_count\":" + likeCount + "}";
    }

    private static String expectedPost(int likeCount) {
        return "{\"post\":" + postJson(likeCount) + "}";
    }
}
