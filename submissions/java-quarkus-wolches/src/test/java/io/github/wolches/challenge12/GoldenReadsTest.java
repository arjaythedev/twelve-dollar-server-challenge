package io.github.wolches.challenge12;

import io.github.wolches.challenge12.support.ApiTestBase;
import io.github.wolches.challenge12.support.TestDatabaseResource;
import io.quarkus.test.common.QuarkusTestResource;
import io.quarkus.test.junit.QuarkusTest;
import io.restassured.response.Response;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.MethodSource;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.stream.Stream;

/**
 * {@code -- golden reads (fresh database)}: exact response bytes against test/golden/*.json.
 */
@QuarkusTest
@QuarkusTestResource(TestDatabaseResource.class)
@Order(1)
class GoldenReadsTest extends ApiTestBase {

    @Test
    void feedMatchesGolden() {
        Response response = get("/feed");
        expect(response, 200, golden("feed.json"));
        assertJsonContentType(response);
    }

    @ParameterizedTest(name = "GET /posts/{0} == golden")
    @MethodSource("goldenPostIds")
    void postMatchesGolden(String id) {
        expect(get("/posts/" + id), 200, golden("post-" + id + ".json"));
    }

    @Test
    void postContentType() {
        assertJsonContentType(get("/posts/1"));
    }

    static Stream<String> goldenPostIds() throws IOException {
        try (Stream<Path> files = Files.list(goldenDir())) {
            List<String> ids = files
                    .map(p -> p.getFileName().toString())
                    .filter(name -> name.matches("post-\\d+\\.json"))
                    .map(name -> name.substring("post-".length(), name.length() - ".json".length()))
                    .sorted()
                    .toList();
            return ids.stream();
        }
    }
}
