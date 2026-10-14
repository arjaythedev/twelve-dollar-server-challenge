package com.brocchirodrigo.feed.adapter.controller;

import org.springframework.web.reactive.function.server.RequestPredicates;
import org.springframework.web.reactive.function.server.RouterFunction;
import org.springframework.web.reactive.function.server.RouterFunctions;
import org.springframework.web.reactive.function.server.ServerResponse;

public final class FeedRoutes {

    private FeedRoutes() {
    }

    public static RouterFunction<ServerResponse> of(PostController posts, HealthController health, FallbackController fallback) {
        return RouterFunctions.route()
                .GET("/health", _ -> health.health())
                .GET("/feed", _ -> posts.feed())
                .GET("/posts/{id}", posts::post)
                .POST("/posts", posts::create)
                .POST("/posts/{id}/like", posts::like)
                .route(RequestPredicates.all(), _ -> fallback.notFound())
                .build();
    }
}
