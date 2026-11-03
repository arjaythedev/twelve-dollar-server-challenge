/* HS256 bearer token verification for the $12 server challenge feed API.
 * Every request's signature is verified from scratch - nothing about a token is remembered between
 * requests ( challenge rule 5 ). */
#ifndef FEEDAPI_JWT_H_INCLUDED
#define FEEDAPI_JWT_H_INCLUDED

/* Mapped onto the 401 messages the spec asks for, which is why "the token is structurally broken" and
 * "the token is fine but its claims are not" are separate results. Finding the token in the request is
 * not this file's job - AmmServer_APIRequestBearer does that - so "there was no token" is not a result
 * here either. */
enum JwtResult
{
   JWT_OK = 0,
   JWT_INVALID,       /* malformed, not HS256, bad signature, or expired */
   JWT_BAD_PAYLOAD    /* signature and exp are good, but sub/username are not what they must be */
};

struct JwtClaims
{
   long long userId;            /* the integer in "sub" */
   const char * username;       /* points into the caller's scratch buffer, NOT NUL terminated */
   unsigned int usernameLength;
};

/**
 * Verify a bearer token and pull out the claims the API needs. The token is what
 * AmmServer_APIRequestBearer returned, so it is not NUL terminated and the length is not optional.
 * scratch holds the decoded payload that claims->username points into, so it has to stay alive and
 * untouched for as long as the claims are used.
 */
enum JwtResult JwtVerifyToken(
                                const char * token,
                                unsigned int tokenLength,
                                const char * secret,
                                unsigned int secretLength,
                                long long now,
                                struct JwtClaims * claims,
                                char * scratch,
                                unsigned int scratchSize
                              );

#endif
