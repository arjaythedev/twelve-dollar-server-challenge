package com.brocchirodrigo.feed.adapter.presenter.dto;

public record HealthResponse(String status, String db, long uptimeS) {

    public static HealthResponse ok(long uptimeSeconds) {
        return new HealthResponse("ok", "ok", uptimeSeconds);
    }
}
