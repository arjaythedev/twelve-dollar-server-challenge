package com.brocchirodrigo.feed.adapter.presenter;

import com.brocchirodrigo.feed.adapter.presenter.dto.ErrorResponse;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.web.reactive.function.server.ServerResponse;
import reactor.core.publisher.Mono;

public final class Responses {

    private Responses() {
    }

    public static Mono<ServerResponse> ok(Object body) {
        return json(HttpStatus.OK, body);
    }

    public static Mono<ServerResponse> created(Object body) {
        return json(HttpStatus.CREATED, body);
    }

    public static Mono<ServerResponse> error(HttpStatus status, String message) {
        return json(status, new ErrorResponse(message));
    }

    public static Mono<ServerResponse> json(HttpStatus status, Object body) {
        return ServerResponse.status(status).contentType(MediaType.APPLICATION_JSON).bodyValue(body);
    }
}
