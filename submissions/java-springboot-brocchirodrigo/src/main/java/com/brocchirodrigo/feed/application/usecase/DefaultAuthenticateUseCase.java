package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.gateway.TokenClaims;
import com.brocchirodrigo.feed.application.gateway.TokenVerifier;
import com.brocchirodrigo.feed.application.usecase.output.AuthRejected;
import com.brocchirodrigo.feed.application.usecase.output.AuthResult;
import com.brocchirodrigo.feed.application.usecase.output.Authenticated;
import com.brocchirodrigo.feed.domain.model.AuthenticatedUser;
import com.brocchirodrigo.feed.domain.model.PositiveId;

public record DefaultAuthenticateUseCase(TokenVerifier verifier) implements AuthenticateUseCase {

    private static final String BEARER = "Bearer ";

    @Override
    public AuthResult execute(String authorization) {
        if (authorization == null || !authorization.startsWith(BEARER)) {
            return AuthRejected.MISSING_BEARER_TOKEN;
        }
        return verifier.verify(authorization.substring(BEARER.length()))
                .map(DefaultAuthenticateUseCase::authenticate)
                .orElse(AuthRejected.INVALID_TOKEN);
    }

    private static AuthResult authenticate(TokenClaims claims) {
        var userId = PositiveId.parse(claims.subject());
        if (userId == PositiveId.INVALID || claims.username() == null) {
            return AuthRejected.INVALID_PAYLOAD;
        }
        return new Authenticated(new AuthenticatedUser(userId, claims.username()));
    }
}
