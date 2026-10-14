package com.brocchirodrigo.feed.infrastructure.config;

import com.brocchirodrigo.feed.application.gateway.TokenVerifier;
import com.brocchirodrigo.feed.application.usecase.AuthenticateUseCase;
import com.brocchirodrigo.feed.application.usecase.CheckHealthUseCase;
import com.brocchirodrigo.feed.application.usecase.CreatePostUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultAuthenticateUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultCheckHealthUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultCreatePostUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultGetFeedUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultGetPostUseCase;
import com.brocchirodrigo.feed.application.usecase.DefaultLikePostUseCase;
import com.brocchirodrigo.feed.application.usecase.GetFeedUseCase;
import com.brocchirodrigo.feed.application.usecase.GetPostUseCase;
import com.brocchirodrigo.feed.application.usecase.LikePostUseCase;
import com.brocchirodrigo.feed.domain.repository.HealthRepository;
import com.brocchirodrigo.feed.domain.repository.PostRepository;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import java.time.InstantSource;

@Configuration(proxyBeanMethods = false)
public class UseCaseConfig {

    @Bean
    GetFeedUseCase getFeedUseCase(PostRepository posts) {
        return new DefaultGetFeedUseCase(posts);
    }

    @Bean
    GetPostUseCase getPostUseCase(PostRepository posts) {
        return new DefaultGetPostUseCase(posts);
    }

    @Bean
    CreatePostUseCase createPostUseCase(PostRepository posts) {
        return new DefaultCreatePostUseCase(posts);
    }

    @Bean
    LikePostUseCase likePostUseCase(PostRepository posts) {
        return new DefaultLikePostUseCase(posts);
    }

    @Bean
    AuthenticateUseCase authenticateUseCase(TokenVerifier verifier) {
        return new DefaultAuthenticateUseCase(verifier);
    }

    @Bean
    CheckHealthUseCase checkHealthUseCase(HealthRepository repository) {
        var clock = InstantSource.system();
        return new DefaultCheckHealthUseCase(repository, clock, clock.instant());
    }
}
