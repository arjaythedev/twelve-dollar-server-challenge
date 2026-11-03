package com.dglynch.twelvedollar;

public sealed interface HealthStatus {
    record Ok(long uptime) implements HealthStatus {
    }

    record DatabaseFailure() implements HealthStatus {
    }

    record NotStarted() implements HealthStatus {
    }
}
