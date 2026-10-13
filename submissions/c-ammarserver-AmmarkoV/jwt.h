/* HS256 bearer token verification for the $12 server challenge feed API.
 * Every request's signature is verified from scratch - nothing about a token is remembered between
 * requests ( challenge rule 5 ). */
#ifndef FEEDAPI_JWT_H_INCLUDED
#define FEEDAPI_JWT_H_INCLUDED

/* Mapped one-to-one onto the three distinct 401 messages the spec asks for, which is why "the token is
 * structurally broken" and "the token is fine but its claims are not" are separate results. */
enum JwtResult
{
   JWT_OK = 0,
   JWT_NO_BEARER,     /* no Authorization header, or it does not start with "Bearer " */
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
 * Verify an Authorization header value and pull out the claims the API needs.
 * authHeader/authHeaderLength is the raw header value ( pass 0 when the request had no such header ).
 * scratch holds the decoded payload that claims->username points into, so it has to stay alive and
 * untouched for as long as the claims are used.
 */
enum JwtResult JwtVerifyBearer(
                                const char * authHeader,
                                unsigned int authHeaderLength,
                                const char * secret,
                                unsigned int secretLength,
                                long long now,
                                struct JwtClaims * claims,
                                char * scratch,
                                unsigned int scratchSize
                              );

#endif
