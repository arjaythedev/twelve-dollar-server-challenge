package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.output.HealthStatus;
import com.brocchirodrigo.feed.application.usecase.output.Healthy;
import com.brocchirodrigo.feed.application.usecase.output.Unhealthy;
import com.brocchirodrigo.feed.domain.exception.PersistenceException;
import com.brocchirodrigo.feed.domain.repository.HealthRepository;

import java.time.Duration;
import java.time.Instant;
import java.time.InstantSource;

public record DefaultCheckHealthUseCase(HealthRepository repository, InstantSource clock, Instant startedAt)
        implements CheckHealthUseCase {

    @Override
    public HealthStatus execute() {
        try {
            repository.ping();
        } catch (PersistenceException error) {
            return new Unhealthy(error.getMessage());
        }
        return new Healthy(Duration.between(startedAt, clock.instant()).toSeconds());
    }
}
