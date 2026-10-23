package io.github.wolches.challenge12.support;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import io.restassured.RestAssured;
import io.restassured.config.DecoderConfig;
import io.restassured.config.EncoderConfig;
import io.restassured.response.Response;
import io.restassured.specification.RequestSpecification;
import org.junit.jupiter.api.BeforeAll;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;

import static io.restassured.RestAssured.given;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Request helpers shared by the suite: {@code req}, {@code expect} and {@code json_ctype} from test/test.sh.
 */
public abstract class ApiTestBase {

    protected static final ObjectMapper JSON = new ObjectMapper();
    protected static final String NO_AUTH = null;
    protected static final String NO_BODY = null;

    @BeforeAll
    static void configureRestAssured() {
        RestAssured.config = RestAssured.config()
                .encoderConfig(EncoderConfig.encoderConfig()
                        .defaultContentCharset("UTF-8")
                        .appendDefaultContentCharsetToContentTypeIfUndefined(false))
                .decoderConfig(DecoderConfig.decoderConfig().defaultContentCharset("UTF-8"));
        RestAssured.urlEncodingEnabled = false;
    }

    /** {@code req GET <path>}. */
    protected static Response get(String path) {
        return given().when().get(path);
    }

    /** {@code req POST <path> [authorization] [raw-body]}: null authorization = no header, null body = no body. */
    protected static Response post(String path, String authorization, String rawJsonBody) {
        RequestSpecification spec = given();
        if (authorization != null) {
            spec = spec.header("Authorization", authorization);
        }
        if (rawJsonBody != null) {
            spec = spec.header("Content-Type", "application/json").body(rawJsonBody);
        }
        return spec.when().post(path);
    }

    /** {@code expect <label> <status> <exact body>}: one trailing newline in the response is tolerated. */
    protected static void expect(Response response, int status, String exactBody) {
        String body = body(response);
        assertEquals(status + " " + exactBody, response.statusCode() + " " + body,
                "want " + status + " " + exactBody + "\ngot  " + response.statusCode() + " " + body);
    }

    /** {@code json_ctype}: the Content-Type must be application/json, a charset parameter is fine. */
    protected static void assertJsonContentType(Response response) {
        String contentType = response.contentType();
        assertTrue(contentType != null && contentType.startsWith("application/json"),
                "content-type: got '" + contentType + "'");
    }

    protected static String body(Response response) {
        String body = response.asString();
        return body.endsWith("\n") ? body.substring(0, body.length() - 1) : body;
    }

    protected static JsonNode json(Response response) {
        try {
            return JSON.readTree(body(response));
        } catch (IOException e) {
            throw new AssertionError("response is not JSON: " + body(response), e);
        }
    }

    protected static Path goldenDir() {
        return TestDatabaseResource.root().resolve("test").resolve("golden");
    }

    protected static String golden(String fileName) {
        try {
            String content = Files.readString(goldenDir().resolve(fileName));
            return content.endsWith("\n") ? content.substring(0, content.length() - 1) : content;
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    protected static JsonNode goldenJson(String fileName) {
        try {
            return JSON.readTree(golden(fileName));
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }
}
