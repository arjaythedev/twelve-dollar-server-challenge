package challenge;

import java.util.Map;
import org.springframework.core.io.buffer.DataBuffer;
import org.springframework.core.io.buffer.DataBufferLimitException;
import org.springframework.core.io.buffer.DataBufferUtils;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.http.server.reactive.ServerHttpRequest;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RestController;
import reactor.core.publisher.Mono;
import tools.jackson.databind.JsonNode;
import tools.jackson.databind.json.JsonMapper;

@RestController
final class FeedController {
    private static final int MAX_BODY_BYTES = 16 * 1024;

    private final PostRepository posts;
    private final Authentication authentication;
    private final JsonMapper json;
    private final long started = System.nanoTime();

    FeedController(PostRepository posts, Authentication authentication, JsonMapper json) {
        this.posts = posts;
        this.authentication = authentication;
        this.json = json;
    }

    @GetMapping("/health")
    Mono<ResponseEntity<Object>> health() {
        return posts.checkHealth()
                .then(
                        Mono.fromSupplier(
                                () ->
                                        ResponseEntity.<Object>ok(
                                                new Responses.Health(
                                                        "ok",
                                                        "ok",
                                                        (System.nanoTime() - started)
                                                                / 1_000_000_000))))
                .onErrorResume(
                        RuntimeException.class,
                        exception ->
                                Mono.just(
                                        ResponseEntity.status(503)
                                                .body(
                                                        new Responses.DegradedHealth(
                                                                "degraded",
                                                                "unreachable",
                                                                "database unavailable"))));
    }

    @GetMapping("/feed")
    Mono<ResponseEntity<byte[]>> feed() {
        return posts.feed().map(FeedController::json);
    }

    @GetMapping("/posts/{id}")
    Mono<ResponseEntity<byte[]>> get(@PathVariable String id) {
        return posts.get(postId(id)).map(FeedController::json);
    }

    @PostMapping("/posts")
    Mono<ResponseEntity<Map<String, ?>>> create(ServerHttpRequest request) {
        Authentication.User user =
                authentication.authenticate(request.getHeaders().getFirst("Authorization"));
        return DataBufferUtils.join(request.getBody(), MAX_BODY_BYTES)
                .map(FeedController::bytes)
                .defaultIfEmpty(new byte[0])
                .onErrorMap(
                        DataBufferLimitException.class,
                        exception -> new ApiException(413, "request body too large"))
                .flatMap(encoded -> posts.create(user, body(encoded)))
                .map(post -> ResponseEntity.status(201).body(Map.of("post", post)));
    }

    @PostMapping("/posts/{id}/like")
    Mono<ResponseEntity<Responses.Like>> like(@PathVariable String id, ServerHttpRequest request) {
        Authentication.User user =
                authentication.authenticate(request.getHeaders().getFirst("Authorization"));
        long postId = postId(id);
        return posts.like(user.id(), postId)
                .map(
                        alreadyLiked ->
                                ResponseEntity.status(alreadyLiked ? 200 : 201)
                                        .body(new Responses.Like(true, alreadyLiked, postId)));
    }

    private String body(byte[] encoded) {
        JsonNode payload;
        try {
            payload = json.readTree(encoded);
            if (payload == null || payload.isMissingNode()) {
                throw new IllegalArgumentException();
            }
        } catch (RuntimeException exception) {
            throw new ApiException(400, "malformed JSON body");
        }
        JsonNode value = payload.path("body");
        if (!value.isString()) {
            throw new ApiException(400, "body is required");
        }
        String body = value.asString().strip();
        if (body.isEmpty()) {
            throw new ApiException(400, "body is required");
        }
        if (body.codePointCount(0, body.length()) > 500) {
            throw new ApiException(400, "body must be at most 500 characters");
        }
        return body;
    }

    /** A response whose JSON body SQLite already built. */
    private static ResponseEntity<byte[]> json(byte[] body) {
        return ResponseEntity.ok().contentType(MediaType.APPLICATION_JSON).body(body);
    }

    private static byte[] bytes(DataBuffer buffer) {
        try {
            byte[] bytes = new byte[buffer.readableByteCount()];
            buffer.read(bytes);
            return bytes;
        } finally {
            DataBufferUtils.release(buffer);
        }
    }

    private long postId(String text) {
        try {
            return Authentication.positiveInteger(text);
        } catch (IllegalArgumentException exception) {
            throw new ApiException(400, "invalid post id");
        }
    }
}
