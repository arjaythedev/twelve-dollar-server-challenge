package com.dglynch.twelvedollar;

import module java.base;
import io.helidon.http.HeaderNames;
import io.helidon.http.media.json.binding.JsonBindingSupport;
import io.helidon.json.JsonException;
import io.helidon.json.JsonParser;
import io.helidon.json.JsonValueType;
import io.helidon.json.binding.JsonBinding;
import io.helidon.webserver.WebServer;
import io.helidon.webserver.http.HttpRouting;
import io.helidon.webserver.http.ServerRequest;
import io.helidon.webserver.http.ServerResponse;

public class WebService {
    private static final String DATABASE_FAILURE = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database failure\"}";
    private static final String NOT_STARTED = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"not started\"}";
    private static final String POST_NOT_FOUND_JSON = "{\"error\":\"post not found\"}";
    private static final String INVALID_POST_ID_JSON = "{\"error\":\"invalid post id\"}";
    private static final String NOT_FOUND_JSON = "{\"error\":\"not found\"}";
    private static final String MISSING_BEARER_TOKEN_JSON = "{\"error\":\"missing bearer token\"}";
    private static final String INVALID_EXPIRED_TOKEN_JSON = "{\"error\":\"invalid or expired token\"}";
    private static final String INVALID_TOKEN_PAYLOAD_JSON = "{\"error\":\"invalid token payload\"}";
    private static final String INVALID_BODY_LENGTH_JSON = "{\"error\":\"body must be at most 500 characters\"}";
    private static final String BODY_IS_REQUIRED_JSON = "{\"error\":\"body is required\"}";
    private static final String MALFORMED_JSON_BODY_JSON = "{\"error\":\"malformed JSON body\"}";

    private static final String INTERNAL_SERVER_ERROR_JSON = "{\"error\":\"internal server error\"}";

    private final BusinessLogic businessLogic;
    private final TokenVerifier tokenVerifier;
    private final String host;
    private final int port;

    private final JsonBinding jsonBinding = JsonBinding.create();

    public WebService(BusinessLogic businessLogic, TokenVerifier tokenVerifier, String host, int port) {
        this.businessLogic = businessLogic;
        this.tokenVerifier = tokenVerifier;
        this.host = host;
        this.port = port;
    }

    public void start() {
        WebServer.builder()
                .host(host)
                .port(port)
                .mediaContext(media -> media.addMediaSupport(JsonBindingSupport.create()))
                .addRouting(HttpRouting.builder()
                        .get("/health", this::health)
                        .get("/feed", this::feed)
                        .get("/posts/{id}", this::getPosts)
                        .post("/posts", this::postPosts)
                        .post("/posts/{id}/like", this::postPostsLike)
                        .any("/*", this::notFound)
                        .error(Throwable.class, this::error)
                )
                .build()
                .start();
        businessLogic.start();
    }

    private void health(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");
        var health = businessLogic.health();
        switch (health) {
            case HealthStatus.Ok ok ->
                    response.status(200).send("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + ok.uptime() + "}");
            case HealthStatus.DatabaseFailure _ -> response.status(503).send(DATABASE_FAILURE);
            case HealthStatus.NotStarted _ -> response.status(503).send(NOT_STARTED);
        }
    }

    private void feed(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");
        response.send("{\"posts\":" + jsonBinding.serialize(businessLogic.feed()) + "}");
    }

    private void getPosts(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");
        try {
            long id = Long.parseLong(request.path().pathParameters().get("id"));
            if (id > 0) {
                var post = businessLogic.getPost(id);
                if (post != null) {
                    response.send("{\"post\":" + jsonBinding.serialize(post) + "}");
                } else {
                    response.status(404).send(POST_NOT_FOUND_JSON);
                }
            } else {
                response.status(400).send(INVALID_POST_ID_JSON);
            }
        } catch (NumberFormatException _) {
            response.status(400).send(INVALID_POST_ID_JSON);
        }
    }

    private void postPosts(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");

        var authorization = request.headers().value(HeaderNames.AUTHORIZATION);
        if (authorization.isEmpty() || !authorization.get().startsWith("Bearer ")) {
            response.status(401).send(MISSING_BEARER_TOKEN_JSON);
            return;
        }

        TokenVerifier.VerifiedToken verifiedToken;
        try {
            verifiedToken = tokenVerifier.verifyToken(authorization.get().substring("Bearer ".length()));
        } catch (TokenVerifier.InvalidJwtException _) {
            response.status(401).send(INVALID_EXPIRED_TOKEN_JSON);
            return;
        }

        var authenticatedUser = tokenVerifier.authenticatedUser(verifiedToken);
        if (authenticatedUser.isEmpty()) {
            response.status(401).send(INVALID_TOKEN_PAYLOAD_JSON);
            return;
        }

        try {
            var jsonObject = JsonParser.create(request.content().inputStream()).readJsonObject();
            var bodyValue = jsonObject.value("body").orElse(null);
            if (bodyValue != null && bodyValue.type() == JsonValueType.STRING && !bodyValue.asString().value().isBlank()) {
                String trimmedBody = bodyValue.asString().value().strip();
                if (trimmedBody.length() <= 500) {
                    var post = businessLogic.createPost(authenticatedUser.get().userId(),
                            authenticatedUser.get().username(), trimmedBody);
                    if (post != null) {
                        response.status(201).send("{\"post\":" + jsonBinding.serialize(post) + "}");
                    } else {
                        response.status(500).send(INTERNAL_SERVER_ERROR_JSON);
                    }
                } else {
                    response.status(400).send(INVALID_BODY_LENGTH_JSON);
                }
            } else {
                response.status(400).send(BODY_IS_REQUIRED_JSON);
            }
        } catch (JsonException _) {
            response.status(400).send(MALFORMED_JSON_BODY_JSON);
        }
    }

    private void postPostsLike(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");

        var authorization = request.headers().value(HeaderNames.AUTHORIZATION);
        if (authorization.isEmpty() || !authorization.get().startsWith("Bearer ")) {
            response.status(401).send(MISSING_BEARER_TOKEN_JSON);
            return;
        }

        TokenVerifier.VerifiedToken verifiedToken;
        try {
            verifiedToken = tokenVerifier.verifyToken(authorization.get().substring("Bearer ".length()));
        } catch (TokenVerifier.InvalidJwtException _) {
            response.status(401).send(INVALID_EXPIRED_TOKEN_JSON);
            return;
        }

        var authenticatedUser = tokenVerifier.authenticatedUser(verifiedToken);
        if (authenticatedUser.isEmpty()) {
            response.status(401).send(INVALID_TOKEN_PAYLOAD_JSON);
            return;
        }

        try {
            long id = Long.parseLong(request.path().pathParameters().get("id"));
            if (id > 0) {
                LikeResult result = businessLogic.likePost(id, authenticatedUser.get().userId());
                switch (result) {
                    case LikeResult.NewlyLiked _ ->
                            response.status(201).send("{\"liked\":true,\"already_liked\":false,\"post_id\":" + id + "}");
                    case LikeResult.AlreadyLiked _ ->
                            response.status(200).send("{\"liked\":true,\"already_liked\":true,\"post_id\":" + id + "}");
                    case LikeResult.PostNotFound _ -> response.status(404).send(POST_NOT_FOUND_JSON);
                    case null, default -> response.status(500).send(INTERNAL_SERVER_ERROR_JSON);
                }
            } else {
                response.status(400).send(INVALID_POST_ID_JSON);
            }
        } catch (NumberFormatException _) {
            response.status(400).send(INVALID_POST_ID_JSON);
        }
    }

    private void notFound(ServerRequest request, ServerResponse response) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");
        response.status(404).send(NOT_FOUND_JSON);
    }

    private void error(ServerRequest request, ServerResponse response, Throwable throwable) {
        response.headers().add(HeaderNames.CONTENT_TYPE, "application/json");
        response.status(500).send(INTERNAL_SERVER_ERROR_JSON);
    }
}
