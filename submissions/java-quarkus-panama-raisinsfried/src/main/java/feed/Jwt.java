package feed;

import com.auth0.jwt.JWT;
import com.auth0.jwt.algorithms.Algorithm;
import com.auth0.jwt.exceptions.JWTVerificationException;
import com.auth0.jwt.interfaces.DecodedJWT;
import com.auth0.jwt.interfaces.JWTVerifier;
import jakarta.inject.Singleton;
import org.eclipse.microprofile.config.inject.ConfigProperty;

/** HS256 only and {@code exp} required; {@code sub} must be a positive integer string, {@code username} a string. */
@Singleton
final class Jwt {
    record User(long id, String username) {}

    private final JWTVerifier verifier;

    Jwt(@ConfigProperty(name = "feed.jwt-secret") String secret) {
        verifier = JWT.require(Algorithm.HMAC256(secret)).withClaimPresence("exp").ignoreIssuedAt().build();
    }

    User verify(String authorization) {
        if (authorization == null || !authorization.startsWith("Bearer ")) throw new ApiError(401, "missing bearer token");
        DecodedJWT jwt;
        try {
            jwt = verifier.verify(authorization.substring("Bearer ".length()));
        } catch (JWTVerificationException e) {
            throw new ApiError(401, "invalid or expired token");
        }
        String sub = jwt.getClaim("sub").asString(), username = jwt.getClaim("username").asString(); // null unless strings
        long id = sub == null ? -1 : FeedResource.positiveId(sub);
        if (id <= 0 || username == null) throw new ApiError(401, "invalid token payload");
        return new User(id, username);
    }
}
