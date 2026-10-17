package com.brocchirodrigo.feed.infrastructure.config;

import com.brocchirodrigo.feed.adapter.controller.FallbackController;
import com.brocchirodrigo.feed.adapter.controller.FeedRoutes;
import com.brocchirodrigo.feed.adapter.controller.HealthController;
import com.brocchirodrigo.feed.adapter.controller.PostController;
import com.brocchirodrigo.feed.application.usecase.AuthenticateUseCase;
import com.brocchirodrigo.feed.application.usecase.CheckHealthUseCase;
import com.brocchirodrigo.feed.application.usecase.CreatePostUseCase;
import com.brocchirodrigo.feed.application.usecase.GetFeedUseCase;
import com.brocchirodrigo.feed.application.usecase.GetPostUseCase;
import com.brocchirodrigo.feed.application.usecase.LikePostUseCase;
import com.brocchirodrigo.feed.infrastructure.exception.GlobalErrorHandler;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.reactive.function.server.RouterFunction;
import org.springframework.web.reactive.function.server.ServerResponse;
import tools.jackson.databind.json.JsonMapper;

@Configuration(proxyBeanMethods = false)
public class WebConfig {

    @Bean
    PostController postController(
            GetFeedUseCase getFeed,
            GetPostUseCase getPost,
            CreatePostUseCase createPost,
            LikePostUseCase likePost,
            AuthenticateUseCase authenticate,
            JsonMapper json) {
        return new PostController(getFeed, getPost, createPost, likePost, authenticate, json);
    }

    @Bean
    HealthController healthController(CheckHealthUseCase checkHealth) {
        return new HealthController(checkHealth);
    }

    @Bean
    FallbackController fallbackController() {
        return new FallbackController();
    }

    @Bean
    RouterFunction<ServerResponse> routes(PostController posts, HealthController health, FallbackController fallback) {
        return FeedRoutes.of(posts, health, fallback);
    }

    @Bean
    GlobalErrorHandler globalErrorHandler(JsonMapper json) {
        return new GlobalErrorHandler(json);
    }
}
