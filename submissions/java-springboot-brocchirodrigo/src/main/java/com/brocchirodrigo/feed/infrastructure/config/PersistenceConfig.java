package com.brocchirodrigo.feed.infrastructure.config;

import com.brocchirodrigo.feed.domain.repository.HealthRepository;
import com.brocchirodrigo.feed.domain.repository.PostRepository;
import com.brocchirodrigo.feed.infrastructure.persistence.repository.SqliteHealthRepository;
import com.brocchirodrigo.feed.infrastructure.persistence.repository.SqlitePostRepository;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteDatabase;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration(proxyBeanMethods = false)
public class PersistenceConfig {

    @Bean
    SqliteDatabase sqliteDatabase(FeedProperties properties) {
        return new SqliteDatabase(properties.sqlitePath());
    }

    @Bean
    PostRepository postRepository(SqliteDatabase database) {
        return new SqlitePostRepository(database);
    }

    @Bean
    HealthRepository healthRepository(SqliteDatabase database) {
        return new SqliteHealthRepository(database);
    }
}
