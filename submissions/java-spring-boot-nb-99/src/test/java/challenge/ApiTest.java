package challenge;

import static org.junit.jupiter.api.Assertions.*;

import com.zaxxer.hikari.HikariDataSource;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.DriverManager;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import javax.sql.DataSource;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.web.server.LocalServerPort;
import org.springframework.test.annotation.DirtiesContext;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import tools.jackson.databind.json.JsonMapper;

@SpringBootTest(
        webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT,
        properties = {"JWT_SECRET=test-secret", "HOST=127.0.0.1"})
class ApiTest {
    private static final Path DATABASE = database();
    private final HttpClient client = HttpClient.newHttpClient();

    @LocalServerPort int port;
    @Autowired JsonMapper json;
    @Autowired DataSource dataSource;

    @DynamicPropertySource
    static void configuration(DynamicPropertyRegistry properties) {
        properties.add("SQLITE_PATH", DATABASE::toString);
    }

    @Test
    void healthAndUnknownPath() throws Exception {
        HttpResponse<String> health = request("GET", "/health", null, null);
        assertEquals(200, health.statusCode());
        assertTrue(
                health.body().matches("\\{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":[0-9]+}"));
        expect(request("GET", "/unknown", null, null), 404, "{\"error\":\"not found\"}");
        expect(request("GET", "/error", null, null), 404, "{\"error\":\"not found\"}");
    }

    @Test
    void authPrecedesBodyAndIdValidation() throws Exception {
        expect(
                request("POST", "/posts", "{bad json", null),
                401,
                "{\"error\":\"missing bearer token\"}");
        expect(
                request("POST", "/posts/abc/like", null, null),
                401,
                "{\"error\":\"missing bearer token\"}");
        expect(
                request("POST", "/posts/abc/like", null, token()),
                400,
                "{\"error\":\"invalid post id\"}");
    }

    @ParameterizedTest
    @ValueSource(strings = {"", "{", "{} {}", "{\"body\":\"x\"} trailing"})
    void rejectsMalformedJson(String body) throws Exception {
        expect(
                request("POST", "/posts", body, token()),
                400,
                "{\"error\":\"malformed JSON body\"}");
    }

    @ParameterizedTest
    @ValueSource(
            strings = {
                "null",
                "[]",
                "42",
                "{}",
                "{\"body\":null}",
                "{\"body\":true}",
                "{\"body\":\"\\t\\n \"}"
            })
    void requiresStringBody(String body) throws Exception {
        expect(request("POST", "/posts", body, token()), 400, "{\"error\":\"body is required\"}");
    }

    @ParameterizedTest
    @ValueSource(strings = {"0", "-1", "1.5", "abc", "9223372036854775808"})
    void validatesIds(String id) throws Exception {
        expect(request("GET", "/posts/" + id, null, null), 400, "{\"error\":\"invalid post id\"}");
    }

    @Test
    void verifiesSignatureAlgorithmExpirationAndClaimTypes() throws Exception {
        long future = Instant.now().getEpochSecond() + 3600;
        List<String> invalidTokens =
                List.of(
                        "abc",
                        token() + "x",
                        sign(
                                "{\"alg\":\"none\"}",
                                "{\"sub\":\"1\",\"username\":\"one\",\"exp\":" + future + "}"),
                        sign(
                                "{\"alg\":\"HS256\"}",
                                "{\"sub\":\"1\",\"username\":\"one\",\"exp\":1}"),
                        sign("{\"alg\":\"HS256\"}", "{\"sub\":\"1\",\"username\":\"one\"}"));
        for (String token : invalidTokens) {
            expect(
                    request("POST", "/posts", "{\"body\":\"x\"}", token),
                    401,
                    "{\"error\":\"invalid or expired token\"}");
        }
        for (String claims :
                List.of(
                        "\"sub\":1,\"username\":\"one\"",
                        "\"sub\":\"0\",\"username\":\"one\"",
                        "\"sub\":\" 1\",\"username\":\"one\"",
                        "\"sub\":\"1\",\"username\":1",
                        "\"sub\":\"1\"")) {
            String token = sign("{\"alg\":\"HS256\"}", "{" + claims + ",\"exp\":" + future + "}");
            expect(
                    request("POST", "/posts", "{\"body\":\"x\"}", token),
                    401,
                    "{\"error\":\"invalid token payload\"}");
        }
    }

    @Test
    void createsUnicodePostAndCommitsBeforeResponding() throws Exception {
        String body = "😀".repeat(500);
        HttpResponse<String> created =
                request(
                        "POST",
                        "/posts",
                        json.writeValueAsString(java.util.Map.of("body", "  " + body + "\n")),
                        token());
        assertEquals(201, created.statusCode());
        var post = json.readTree(created.body()).path("post");
        assertEquals(body, post.path("body").asString());
        assertEquals("token-name", post.path("author").asString());
        assertTrue(post.path("created_at").asString().matches(".*\\.[0-9]{3}Z"));
        try (var connection = DriverManager.getConnection("jdbc:sqlite:" + DATABASE);
                var statement =
                        connection.prepareStatement("SELECT body FROM posts WHERE id = ?")) {
            statement.setLong(1, post.path("id").asLong());
            try (var rows = statement.executeQuery()) {
                assertTrue(rows.next());
                assertEquals(body, rows.getString(1));
            }
        }
        expect(
                request(
                        "POST",
                        "/posts",
                        json.writeValueAsString(java.util.Map.of("body", body + "😀")),
                        token()),
                400,
                "{\"error\":\"body must be at most 500 characters\"}");
    }

    @Test
    void concurrentLikesAreIdempotentAndVisible() throws Exception {
        HttpResponse<String> created = request("POST", "/posts", "{\"body\":\"like me\"}", token());
        assertEquals(201, created.statusCode());
        long id = json.readTree(created.body()).path("post").path("id").asLong();
        String token = token();
        List<CompletableFuture<HttpResponse<String>>> likes = new ArrayList<>();
        for (int i = 0; i < 8; i++) {
            likes.add(
                    client.sendAsync(
                            httpRequest("POST", "/posts/" + id + "/like", null, token),
                            HttpResponse.BodyHandlers.ofString()));
        }
        int inserted = 0;
        for (var future : likes) {
            HttpResponse<String> response = future.get();
            if (response.statusCode() == 201) inserted++;
            assertTrue(response.statusCode() == 200 || response.statusCode() == 201);
            assertEquals(
                    "{\"liked\":true,\"already_liked\":"
                            + (response.statusCode() == 200)
                            + ",\"post_id\":"
                            + id
                            + "}",
                    response.body());
        }
        assertEquals(1, inserted);
        HttpResponse<String> read = request("GET", "/posts/" + id, null, null);
        assertEquals(200, read.statusCode());
        assertEquals(1, json.readTree(read.body()).path("post").path("like_count").asInt());
        String timestamp = json.readTree(created.body()).path("post").path("created_at").asString();
        assertEquals(
                "{\"post\":{\"id\":"
                        + id
                        + ",\"body\":\"like me\",\"created_at\":\""
                        + timestamp
                        + "\",\"author\":\"database-name\",\"like_count\":1}}",
                read.body());
        try (var connection = DriverManager.getConnection("jdbc:sqlite:" + DATABASE);
                var statement =
                        connection.prepareStatement(
                                "SELECT count(*) FROM likes WHERE post_id = ?")) {
            statement.setLong(1, id);
            try (var rows = statement.executeQuery()) {
                assertTrue(rows.next());
                assertEquals(1, rows.getInt(1));
            }
        }
        expect(
                request("POST", "/posts/999999999/like", null, token),
                404,
                "{\"error\":\"post not found\"}");
    }

    @Test
    void enablesRequiredDatabasePragmas() throws Exception {
        try (var first = dataSource.getConnection();
                var second = dataSource.getConnection();
                var third = dataSource.getConnection();
                var fourth = dataSource.getConnection()) {
            for (var connection : List.of(first, second, third, fourth)) {
                for (var pragma :
                        java.util.Map.of(
                                        "journal_mode",
                                        "wal",
                                        "synchronous",
                                        "1",
                                        "foreign_keys",
                                        "1",
                                        "busy_timeout",
                                        "5000")
                                .entrySet()) {
                    try (var statement = connection.createStatement();
                            var rows = statement.executeQuery("PRAGMA " + pragma.getKey())) {
                        assertTrue(rows.next());
                        assertEquals(pragma.getValue(), rows.getString(1), pragma.getKey());
                    }
                }
            }
        }
    }

    @Test
    void feedOrdersTimestampThenIdAndLimitsToTwenty() throws Exception {
        try (var connection = DriverManager.getConnection("jdbc:sqlite:" + DATABASE);
                var statement =
                        connection.prepareStatement(
                                "INSERT INTO posts (id, user_id, body, created_at) VALUES (?, 1, 'tie', ?)")) {
            for (int i = 1; i <= 21; i++) {
                statement.setLong(1, 1_000_000 + i);
                statement.setString(
                        2, i == 21 ? "9998-01-01T00:00:00.000Z" : "9999-01-01T00:00:00.000Z");
                statement.executeUpdate();
            }
        }
        try {
            List<String> expected = new ArrayList<>();
            for (int i = 20; i >= 1; i--) {
                expected.add(
                        "{\"id\":"
                                + (1_000_000 + i)
                                + ",\"body\":\"tie\",\"created_at\":\"9999-01-01T00:00:00.000Z\","
                                + "\"author\":\"database-name\",\"like_count\":0}");
            }
            expect(
                    request("GET", "/feed", null, null),
                    200,
                    "{\"posts\":[" + String.join(",", expected) + "]}");
        } finally {
            try (var connection = DriverManager.getConnection("jdbc:sqlite:" + DATABASE);
                    var statement = connection.createStatement()) {
                statement.executeUpdate("DELETE FROM posts WHERE id BETWEEN 1000001 AND 1000021");
            }
        }
    }

    @Test
    @DirtiesContext
    void unhealthyDatabaseReturns503() throws Exception {
        ((HikariDataSource) dataSource).close();
        expect(
                request("GET", "/health", null, null),
                503,
                "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");
    }

    @Test
    void boundsRequestBodyAfterAuthentication() throws Exception {
        String body = " ".repeat(16 * 1024 + 1);
        expect(
                request("POST", "/posts", body, token()),
                413,
                "{\"error\":\"request body too large\"}");
        expect(request("POST", "/posts", body, null), 401, "{\"error\":\"missing bearer token\"}");
    }

    private HttpResponse<String> request(String method, String path, String body, String token)
            throws Exception {
        return client.send(
                httpRequest(method, path, body, token), HttpResponse.BodyHandlers.ofString());
    }

    private HttpRequest httpRequest(String method, String path, String body, String token) {
        var request = HttpRequest.newBuilder(URI.create("http://127.0.0.1:" + port + path));
        if (token != null) request.header("Authorization", "Bearer " + token);
        if (body != null) request.header("Content-Type", "application/json");
        return request.method(
                        method,
                        body == null
                                ? HttpRequest.BodyPublishers.noBody()
                                : HttpRequest.BodyPublishers.ofString(body))
                .build();
    }

    private void expect(HttpResponse<String> response, int status, String body) {
        assertEquals(status, response.statusCode(), response.body());
        assertEquals(body, response.body());
        assertTrue(
                response.headers()
                        .firstValue("content-type")
                        .orElse("")
                        .startsWith("application/json"));
    }

    private String token() throws Exception {
        return sign(
                "{\"alg\":\"HS256\"}",
                "{\"sub\":\"1\",\"username\":\"token-name\",\"exp\":"
                        + (Instant.now().getEpochSecond() + 3600)
                        + "}");
    }

    private String sign(String header, String payload) throws Exception {
        Base64.Encoder encoder = Base64.getUrlEncoder().withoutPadding();
        String unsigned =
                encoder.encodeToString(header.getBytes(StandardCharsets.UTF_8))
                        + "."
                        + encoder.encodeToString(payload.getBytes(StandardCharsets.UTF_8));
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec("test-secret".getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        return unsigned
                + "."
                + encoder.encodeToString(mac.doFinal(unsigned.getBytes(StandardCharsets.US_ASCII)));
    }

    private static Path database() {
        try {
            Path file = Files.createTempFile("feed-test-", ".db");
            try (var connection = DriverManager.getConnection("jdbc:sqlite:" + file);
                    var statement = connection.createStatement()) {
                for (String sql : Files.readString(Path.of("../../schema.sql")).split(";")) {
                    if (!sql.isBlank()) statement.execute(sql);
                }
                statement.execute("INSERT INTO users (id, username) VALUES (1, 'database-name')");
            }
            file.toFile().deleteOnExit();
            return file;
        } catch (Exception exception) {
            throw new ExceptionInInitializerError(exception);
        }
    }
}
