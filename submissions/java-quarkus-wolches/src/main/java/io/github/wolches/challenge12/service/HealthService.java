package io.github.wolches.challenge12.service;

import io.github.wolches.challenge12.model.response.HealthResponseDto;
import io.github.wolches.challenge12.dao.AsyncSeparatedReadWriteDao;
import io.smallrye.mutiny.Uni;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.inject.Inject;
import lombok.RequiredArgsConstructor;

import java.lang.management.ManagementFactory;
import java.time.Duration;

@ApplicationScoped
@RequiredArgsConstructor(onConstructor_ = @Inject)
public class HealthService {

    private final AsyncSeparatedReadWriteDao dao;
    public Uni<HealthResponseDto> getHealth() {
        return dao.healthCheck()
           .map(selected -> {
               Duration uptime = Duration.ofMillis(ManagementFactory.getRuntimeMXBean().getUptime());
               return HealthResponseDto.create((int) uptime.toSeconds());
           })
           .onFailure().recoverWithItem(e -> HealthResponseDto.create(e.getMessage()));
    }
}
