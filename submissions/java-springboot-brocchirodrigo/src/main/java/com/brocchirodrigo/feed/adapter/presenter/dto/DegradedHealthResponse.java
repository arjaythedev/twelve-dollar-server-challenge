package com.brocchirodrigo.feed.adapter.presenter.dto;

public record DegradedHealthResponse(String status, String db, String error) {

    public static DegradedHealthResponse unreachable(String error) {
        return new DegradedHealthResponse("degraded", "unreachable", error);
    }
}
