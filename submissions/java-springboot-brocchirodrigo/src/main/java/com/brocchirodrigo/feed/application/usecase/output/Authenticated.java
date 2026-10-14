package com.brocchirodrigo.feed.application.usecase.output;

import com.brocchirodrigo.feed.domain.model.AuthenticatedUser;

public record Authenticated(AuthenticatedUser user) implements AuthResult {
}
