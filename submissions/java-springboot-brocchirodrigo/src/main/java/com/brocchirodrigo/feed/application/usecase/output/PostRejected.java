package com.brocchirodrigo.feed.application.usecase.output;

import com.brocchirodrigo.feed.domain.model.enums.PostBodyViolation;

public record PostRejected(PostBodyViolation violation) implements CreatePostResult {
}
