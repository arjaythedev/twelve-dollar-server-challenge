package com.brocchirodrigo.feed.application.usecase.output;

public record Unhealthy(String error) implements HealthStatus {
}
