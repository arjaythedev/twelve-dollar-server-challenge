package com.brocchirodrigo.feed.application.usecase.output;

import com.brocchirodrigo.feed.domain.model.enums.AuthFailure;

public record AuthRejected(AuthFailure reason) implements AuthResult {

    public static final AuthRejected MISSING_BEARER_TOKEN = new AuthRejected(AuthFailure.MISSING_BEARER_TOKEN);
    public static final AuthRejected INVALID_TOKEN = new AuthRejected(AuthFailure.INVALID_TOKEN);
    public static final AuthRejected INVALID_PAYLOAD = new AuthRejected(AuthFailure.INVALID_PAYLOAD);
}
