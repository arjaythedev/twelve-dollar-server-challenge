package io.github.wolches.challenge12.rest;

import io.github.wolches.challenge12.exception.InvalidTokenPayloadException;
import io.quarkus.security.Authenticated;
import io.quarkus.security.identity.SecurityIdentity;
import jakarta.inject.Inject;
import jakarta.ws.rs.container.ResourceInfo;
import lombok.RequiredArgsConstructor;
import org.eclipse.microprofile.jwt.JsonWebToken;
import org.jboss.resteasy.reactive.server.ServerRequestFilter;

@RequiredArgsConstructor(onConstructor_ = @Inject)
public class TokenPayloadFilter {

    private final JsonWebToken jwt;
    private final SecurityIdentity identity;

    @ServerRequestFilter
    public void validate(ResourceInfo info) {
        if (info.getResourceMethod().isAnnotationPresent(Authenticated.class)) {
            String sub = jwt.getSubject();
            if (sub == null || !sub.matches("[1-9]\\d*")) {
                throw new InvalidTokenPayloadException();
            }
            Object username = jwt.getClaim("username");
            if (!(username instanceof String)) {
                throw new InvalidTokenPayloadException();
            }
        }
    }
}