package io.github.wolches.challenge12.model.response;

import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.annotation.JsonPropertyOrder;

public sealed interface HealthResponseDto permits
        HealthResponseDto.SuccessResponseDto,
        HealthResponseDto.ErrorResponseDto {

    static HealthResponseDto create(String error) {
        return new ErrorResponseDto(error);
    }

    static HealthResponseDto create(int uptime) {
        return new SuccessResponseDto(uptime);
    }

    @JsonPropertyOrder({"status", "db", "uptime_s"})
    record SuccessResponseDto(String status, String db, @JsonProperty("uptime_s") int uptime
    ) implements HealthResponseDto {
        public SuccessResponseDto (int uptime) {
            this("ok", "ok", uptime);
        }
    }

    @JsonPropertyOrder({"status", "db", "error"})
    record ErrorResponseDto(String status, String db, String error) implements HealthResponseDto {
        public ErrorResponseDto(String error) {
            this("degraded", "unreachable", error);
        }
    }

}
