package com.brocchirodrigo.feed.adapter.controller;

import com.brocchirodrigo.feed.adapter.presenter.ErrorMessages;
import com.brocchirodrigo.feed.adapter.presenter.Responses;
import org.springframework.http.HttpStatus;
import org.springframework.web.reactive.function.server.ServerResponse;
import reactor.core.publisher.Mono;

public record FallbackController() {

    public Mono<ServerResponse> notFound() {
        return Responses.error(HttpStatus.NOT_FOUND, ErrorMessages.NOT_FOUND);
    }
}
