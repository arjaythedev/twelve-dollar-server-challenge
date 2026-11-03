package io.github.wolches.challenge12.rest;

import io.github.wolches.challenge12.model.response.HealthResponseDto;
import io.github.wolches.challenge12.service.HealthService;
import io.smallrye.mutiny.Uni;
import jakarta.inject.Inject;
import jakarta.ws.rs.GET;
import jakarta.ws.rs.Path;
import jakarta.ws.rs.Produces;
import jakarta.ws.rs.core.MediaType;
import lombok.RequiredArgsConstructor;

@Path("/health")
@RequiredArgsConstructor(onConstructor_ = @Inject)
public class HealthController {

    private final HealthService healthService;

    @GET
    @Produces(MediaType.APPLICATION_JSON)
    public Uni<HealthResponseDto> health() {
        return healthService.getHealth();
    }
}
