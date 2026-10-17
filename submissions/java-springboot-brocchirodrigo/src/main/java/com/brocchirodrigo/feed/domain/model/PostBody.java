package com.brocchirodrigo.feed.domain.model;

import com.brocchirodrigo.feed.domain.model.enums.PostBodyViolation;

import java.util.Objects;

public record PostBody(String value) implements PostBodyValidation {

    public static final int MAX_LENGTH = 500;

    private static final PostBodyRejected REQUIRED = new PostBodyRejected(PostBodyViolation.REQUIRED);
    private static final PostBodyRejected TOO_LONG = new PostBodyRejected(PostBodyViolation.TOO_LONG);

    public PostBody {
        Objects.requireNonNull(value);
    }

    public static PostBodyValidation of(String raw) {
        var value = raw.strip();
        if (value.isEmpty()) {
            return REQUIRED;
        }
        if (value.length() > MAX_LENGTH && value.codePointCount(0, value.length()) > MAX_LENGTH) {
            return TOO_LONG;
        }
        return new PostBody(value);
    }
}
