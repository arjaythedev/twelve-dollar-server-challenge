package com.brocchirodrigo.feed.infrastructure.config;

import com.brocchirodrigo.feed.application.gateway.TokenVerifier;
import com.brocchirodrigo.feed.infrastructure.security.Hs256TokenVerifier;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import tools.jackson.databind.json.JsonMapper;

@Configuration(proxyBeanMethods = false)
public class SecurityConfig {

    @Bean
    TokenVerifier tokenVerifier(FeedProperties properties, JsonMapper json) {
        return new Hs256TokenVerifier(properties.jwtSecret(), json);
    }
}
