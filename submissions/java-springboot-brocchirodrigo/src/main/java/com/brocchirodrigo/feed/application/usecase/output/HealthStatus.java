package com.brocchirodrigo.feed.application.usecase.output;

public sealed interface HealthStatus permits Healthy, Unhealthy {
}
