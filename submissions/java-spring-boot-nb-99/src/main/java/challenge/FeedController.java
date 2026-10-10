package challenge;

import jakarta.servlet.http.HttpServletRequest;
import java.io.IOException;
import java.util.Map;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RestController;
import tools.jackson.databind.JsonNode;
import tools.jackson.databind.json.JsonMapper;

@RestController
final class FeedController {
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
    ResponseEntity<?> health() {
        try {
            posts.checkHealth();
            return ResponseEntity.ok(
                    new Responses.Health(
                            "ok", "ok", (System.nanoTime() - started) / 1_000_000_000));
        } catch (RuntimeException exception) {
            return ResponseEntity.status(503)
                    .body(
                            new Responses.DegradedHealth(
                                    "degraded", "unreachable", "database unavailable"));
        }
    }

    @GetMapping("/feed")
    Map<String, ?> feed() {
        return Map.of("posts", posts.feed());
    }

    @GetMapping("/posts/{id}")
    Map<String, ?> get(@PathVariable String id) {
        return Map.of("post", posts.get(postId(id)));
    }

    @PostMapping("/posts")
    ResponseEntity<?> create(HttpServletRequest request) {
        Authentication.User user = authentication.authenticate(request.getHeader("Authorization"));
        JsonNode payload;
        try {
            byte[] encoded = request.getInputStream().readNBytes(16 * 1024 + 1);
            if (encoded.length > 16 * 1024) {
                return ResponseEntity.status(413)
                        .body(new Responses.Error("request body too large"));
            }
            payload = json.readTree(encoded);
            if (payload == null || payload.isMissingNode()) {
                throw new IllegalArgumentException();
            }
        } catch (IOException | RuntimeException exception) {
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
        return ResponseEntity.status(201).body(Map.of("post", posts.create(user, body)));
    }

    @PostMapping("/posts/{id}/like")
    ResponseEntity<?> like(@PathVariable String id, HttpServletRequest request) {
        Authentication.User user = authentication.authenticate(request.getHeader("Authorization"));
        long postId = postId(id);
        boolean alreadyLiked = posts.like(user.id(), postId);
        return ResponseEntity.status(alreadyLiked ? 200 : 201)
                .body(new Responses.Like(true, alreadyLiked, postId));
    }

    private long postId(String text) {
        try {
            return Authentication.positiveInteger(text);
        } catch (IllegalArgumentException exception) {
            throw new ApiException(400, "invalid post id");
        }
    }
}
