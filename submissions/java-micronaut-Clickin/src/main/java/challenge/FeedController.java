package challenge;

import io.micronaut.http.*;
import io.micronaut.http.annotation.*;
import io.micronaut.http.annotation.Error;
import io.micronaut.core.annotation.Nullable;
import io.micronaut.json.JsonMapper;
import io.micronaut.json.tree.JsonNode;
import java.util.concurrent.CompletableFuture;
import java.util.function.Supplier;

@Controller
public final class FeedController {
    private final Database db;
    private final DbExecutor executor;
    private final JwtVerifier jwt;
    private final JsonMapper json;
    private final long started = System.nanoTime();
    public FeedController(Database db, DbExecutor executor, JwtVerifier jwt, JsonMapper json) {
        this.db = db; this.executor = executor; this.jwt = jwt; this.json = json;
    }
    private static HttpResponse<?> response(int status, Object body) {
        return HttpResponse.status(HttpStatus.valueOf(status)).body(body).contentType(MediaType.APPLICATION_JSON_TYPE);
    }
    static HttpResponse<?> failure(Throwable e) {
        while (e instanceof java.util.concurrent.CompletionException && e.getCause() != null) e = e.getCause();
        if (e instanceof ApiException api) return response(api.status, new Responses.Error(api.getMessage()));
        if (e instanceof io.micronaut.http.exceptions.HttpStatusException status && status.getStatus() == HttpStatus.NOT_FOUND)
            return response(404, new Responses.Error("not found"));
        return response(500, new Responses.Error("internal server error"));
    }
    private CompletableFuture<HttpResponse<?>> handle(Supplier<CompletableFuture<HttpResponse<?>>> action) {
        // Failures raised before the DB call (auth, validation) become a response here; failures inside the DB call
        // are mapped by DbExecutor itself, so a request creates a single CompletableFuture.
        try { return action.get(); }
        catch (ApiException e) { return CompletableFuture.completedFuture(failure(e)); }
    }
    @Get("/health") public CompletableFuture<HttpResponse<?>> health() {
        return executor.<HttpResponse<?>>read(() -> {
            if (!db.health()) throw new IllegalStateException("SELECT 1 failed");
            return response(200, new Responses.Health("ok", "ok", (System.nanoTime() - started) / 1_000_000_000));
        }, e -> response(503, new Responses.Degraded("degraded", "unreachable", "internal server error")));
    }
    @Get("/feed") public CompletableFuture<HttpResponse<?>> feed() {
        return handle(() -> executor.<HttpResponse<?>>read(() -> response(200, db.feed()), FeedController::failure));
    }
    @Get("/posts/{id}") public CompletableFuture<HttpResponse<?>> post(String id) {
        return handle(() -> {
            long postId = id(id);
            return executor.<HttpResponse<?>>read(() -> response(200, db.find(postId)), FeedController::failure);
        });
    }
    @Post(value="/posts", consumes=MediaType.ALL)
    public CompletableFuture<HttpResponse<?>> create(HttpRequest<?> request, @Nullable @Body String raw) {
        return handle(() -> {
            var user = jwt.verify(request.getHeaders().get("Authorization"));
            String body = body(raw);
            return executor.<HttpResponse<?>>write(() -> response(201, new Responses.Single(db.create(user, body))), FeedController::failure);
        });
    }
    @Post(value="/posts/{id}/like", consumes=MediaType.ALL)
    public CompletableFuture<HttpResponse<?>> like(String id, HttpRequest<?> request) {
        return handle(() -> {
            var user = jwt.verify(request.getHeaders().get("Authorization"));
            long postId = id(id);
            return executor.<HttpResponse<?>>write(() -> {
                boolean already = db.like(user.id(), postId);
                return response(already ? 200 : 201, new Responses.Like(true, already, postId));
            }, FeedController::failure);
        });
    }
    static long id(String value) {
        try {
            for (int i = 0; i < value.length(); i++)
                if (value.charAt(i) < '0' || value.charAt(i) > '9') throw new NumberFormatException();
            long id = Long.parseLong(value);
            if (id > 0) return id;
        } catch (NumberFormatException ignored) {}
        throw new ApiException(400, "invalid post id");
    }
    String body(String raw) {
        JsonNode node;
        try { node = json.readValue(raw == null ? "" : raw, JsonNode.class); }
        catch (Exception e) { throw new ApiException(400, "malformed JSON body"); }
        JsonNode body = node != null && node.isObject() ? node.get("body") : null;
        if (body == null || !body.isString()) throw new ApiException(400, "body is required");
        String value = body.getStringValue().strip();
        if (value.isEmpty()) throw new ApiException(400, "body is required");
        if (value.length() > 500 && value.codePointCount(0, value.length()) > 500)
            throw new ApiException(400, "body must be at most 500 characters");
        return value;
    }
    @Error(global=true)
    public HttpResponse<?> error(Throwable e) { return failure(e); }
}
