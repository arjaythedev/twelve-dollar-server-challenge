package com.brocchirodrigo.feed.adapter.controller;

import com.brocchirodrigo.feed.adapter.controller.dto.CreatePostRequest;
import com.brocchirodrigo.feed.adapter.presenter.ErrorMessages;
import com.brocchirodrigo.feed.adapter.presenter.Responses;
import com.brocchirodrigo.feed.adapter.presenter.dto.FeedResponse;
import com.brocchirodrigo.feed.adapter.presenter.dto.LikeResponse;
import com.brocchirodrigo.feed.adapter.presenter.dto.PostEnvelope;
import com.brocchirodrigo.feed.application.usecase.AuthenticateUseCase;
import com.brocchirodrigo.feed.application.usecase.CreatePostUseCase;
import com.brocchirodrigo.feed.application.usecase.GetFeedUseCase;
import com.brocchirodrigo.feed.application.usecase.GetPostUseCase;
import com.brocchirodrigo.feed.application.usecase.LikePostUseCase;
import com.brocchirodrigo.feed.application.usecase.input.CreatePostCommand;
import com.brocchirodrigo.feed.application.usecase.input.LikePostCommand;
import com.brocchirodrigo.feed.application.usecase.output.AuthRejected;
import com.brocchirodrigo.feed.application.usecase.output.AuthResult;
import com.brocchirodrigo.feed.application.usecase.output.Authenticated;
import com.brocchirodrigo.feed.application.usecase.output.PostCreated;
import com.brocchirodrigo.feed.application.usecase.output.PostFound;
import com.brocchirodrigo.feed.application.usecase.output.PostNotFound;
import com.brocchirodrigo.feed.application.usecase.output.PostRejected;
import com.brocchirodrigo.feed.domain.model.AuthenticatedUser;
import com.brocchirodrigo.feed.domain.model.PositiveId;
import com.brocchirodrigo.feed.domain.model.enums.AuthFailure;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.web.reactive.function.server.ServerRequest;
import org.springframework.web.reactive.function.server.ServerResponse;
import reactor.core.publisher.Mono;
import tools.jackson.core.JacksonException;
import tools.jackson.databind.JsonNode;
import tools.jackson.databind.json.JsonMapper;

import java.util.Optional;

public record PostController(
        GetFeedUseCase getFeed,
        GetPostUseCase getPost,
        CreatePostUseCase createPost,
        LikePostUseCase likePost,
        AuthenticateUseCase authenticate,
        JsonMapper json) {

    public Mono<ServerResponse> feed() {
        return Responses.ok(FeedResponse.from(getFeed.execute()));
    }

    public Mono<ServerResponse> post(ServerRequest request) {
        var id = PositiveId.parse(request.pathVariable("id"));
        if (id == PositiveId.INVALID) {
            return Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.INVALID_POST_ID);
        }
        return switch (getPost.execute(id)) {
            case PostFound(var post) -> Responses.ok(PostEnvelope.from(post));
            case PostNotFound _ -> Responses.error(HttpStatus.NOT_FOUND, ErrorMessages.POST_NOT_FOUND);
        };
    }

    public Mono<ServerResponse> create(ServerRequest request) {
        return switch (authenticationOf(request)) {
            case AuthRejected(var reason) -> unauthorized(reason);
            case Authenticated(var user) -> request.bodyToMono(byte[].class)
                    .flatMap(bytes -> create(user, bytes))
                    .switchIfEmpty(Mono.defer(() -> Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.MALFORMED_JSON)));
        };
    }

    public Mono<ServerResponse> like(ServerRequest request) {
        return switch (authenticationOf(request)) {
            case AuthRejected(var reason) -> unauthorized(reason);
            case Authenticated(var user) -> like(user, request.pathVariable("id"));
        };
    }

    private Mono<ServerResponse> create(AuthenticatedUser user, byte[] bytes) {
        return parse(bytes)
                .map(node -> CreatePostRequest.from(node)
                        .map(request -> create(user, request))
                        .orElseGet(() -> Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.BODY_REQUIRED)))
                .orElseGet(() -> Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.MALFORMED_JSON));
    }

    private Mono<ServerResponse> create(AuthenticatedUser user, CreatePostRequest request) {
        return switch (createPost.execute(new CreatePostCommand(user.id(), user.username(), request.body()))) {
            case PostCreated(var post) -> Responses.created(PostEnvelope.from(post));
            case PostRejected(var violation) -> Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.of(violation));
        };
    }

    private Mono<ServerResponse> like(AuthenticatedUser user, String rawId) {
        var postId = PositiveId.parse(rawId);
        if (postId == PositiveId.INVALID) {
            return Responses.error(HttpStatus.BAD_REQUEST, ErrorMessages.INVALID_POST_ID);
        }
        return switch (likePost.execute(new LikePostCommand(user.id(), postId))) {
            case LIKED -> Responses.created(LikeResponse.liked(postId));
            case ALREADY_LIKED -> Responses.ok(LikeResponse.alreadyLiked(postId));
            case POST_NOT_FOUND -> Responses.error(HttpStatus.NOT_FOUND, ErrorMessages.POST_NOT_FOUND);
        };
    }

    private AuthResult authenticationOf(ServerRequest request) {
        return authenticate.execute(request.headers().firstHeader(HttpHeaders.AUTHORIZATION));
    }

    private static Mono<ServerResponse> unauthorized(AuthFailure failure) {
        return Responses.error(HttpStatus.UNAUTHORIZED, ErrorMessages.of(failure));
    }

    private Optional<JsonNode> parse(byte[] bytes) {
        try {
            return Optional.ofNullable(json.readTree(bytes)).filter(node -> !node.isMissingNode());
        } catch (JacksonException _) {
            return Optional.empty();
        }
    }
}
