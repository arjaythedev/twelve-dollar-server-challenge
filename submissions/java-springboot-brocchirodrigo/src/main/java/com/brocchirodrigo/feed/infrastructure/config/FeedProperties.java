package com.brocchirodrigo.feed.infrastructure.config;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("feed")
public record FeedProperties(String sqlitePath, String jwtSecret) {
}
