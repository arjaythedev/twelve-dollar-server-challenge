package com.brocchirodrigo.feed.adapter.presenter;

import com.brocchirodrigo.feed.domain.model.enums.AuthFailure;
import com.brocchirodrigo.feed.domain.model.enums.PostBodyViolation;

public final class ErrorMessages {

    public static final String NOT_FOUND = "not found";
    public static final String INVALID_POST_ID = "invalid post id";
    public static final String POST_NOT_FOUND = "post not found";
    public static final String MALFORMED_JSON = "malformed JSON body";
    public static final String BODY_REQUIRED = "body is required";
    public static final String INTERNAL = "internal server error";

    private ErrorMessages() {
    }

    public static String of(AuthFailure failure) {
        return switch (failure) {
            case MISSING_BEARER_TOKEN -> "missing bearer token";
            case INVALID_TOKEN -> "invalid or expired token";
            case INVALID_PAYLOAD -> "invalid token payload";
        };
    }

    public static String of(PostBodyViolation violation) {
        return switch (violation) {
            case REQUIRED -> BODY_REQUIRED;
            case TOO_LONG -> "body must be at most 500 characters";
        };
    }
}
