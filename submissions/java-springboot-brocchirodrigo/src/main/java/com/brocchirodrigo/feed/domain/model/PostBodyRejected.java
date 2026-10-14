package com.brocchirodrigo.feed.domain.model;

import com.brocchirodrigo.feed.domain.model.enums.PostBodyViolation;

public record PostBodyRejected(PostBodyViolation violation) implements PostBodyValidation {
}
