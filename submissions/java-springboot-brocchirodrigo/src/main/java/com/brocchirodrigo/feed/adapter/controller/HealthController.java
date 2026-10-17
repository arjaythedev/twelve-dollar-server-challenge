package com.brocchirodrigo.feed.adapter.controller;

import com.brocchirodrigo.feed.adapter.presenter.Responses;
import com.brocchirodrigo.feed.adapter.presenter.dto.DegradedHealthResponse;
import com.brocchirodrigo.feed.adapter.presenter.dto.HealthResponse;
import com.brocchirodrigo.feed.application.usecase.CheckHealthUseCase;
import com.brocchirodrigo.feed.application.usecase.output.Healthy;
import com.brocchirodrigo.feed.application.usecase.output.Unhealthy;
import org.springframework.http.HttpStatus;
import org.springframework.web.reactive.function.server.ServerResponse;
import reactor.core.publisher.Mono;

public record HealthController(CheckHealthUseCase checkHealth) {

    public Mono<ServerResponse> health() {
        return switch (checkHealth.execute()) {
            case Healthy(var uptimeSeconds) -> Responses.ok(HealthResponse.ok(uptimeSeconds));
            case Unhealthy(var error) ->
                    Responses.json(HttpStatus.SERVICE_UNAVAILABLE, DegradedHealthResponse.unreachable(error));
        };
    }
}
