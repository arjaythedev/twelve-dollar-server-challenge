package com.brocchirodrigo.feed.application.gateway;

import java.util.Optional;

public interface TokenVerifier {

    Optional<TokenClaims> verify(String token);
}
