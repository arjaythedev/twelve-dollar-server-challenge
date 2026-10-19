-- Authentication: bearer-token verification through lunet.security's
-- HS256 decoder (RFC 7518 compliant since v0.14.1: the HMAC key is the raw
-- secret octets, so tokens signed by standard libraries verify unchanged).
-- The structural failures (format, base64, signature, header, expired) are
-- one error to the caller; payload TYPE failures are the caller's distinct
-- reason, checked below, per the SPEC's contract.

local config = require("config")
local security = require("lunet.security")

local auth = {}

-- authenticate(ctx) returns the token owner ({ id, username }) or nil plus
-- the rejection reason for error_body.
function auth.authenticate(ctx)
    local authorization = ctx.headers["authorization"]
    if not authorization then
        return nil, "missing bearer token"
    end

    local token = authorization:match("^Bearer%s+([^%s]+)$")
    if not token then
        return nil, "missing bearer token"
    end

    -- Token content is untrusted input: decode reports structural failures
    -- as nil plus a reason; malformed arguments would be calling bugs.
    local claims = security.jwt.decode(token, config.JWT_SECRET)
    if not claims then
        return nil, "invalid or expired token"
    end

    local user_id = type(claims.sub) == "string" and claims.sub:match("^%d+$") and tonumber(claims.sub)
    if not user_id or user_id < 1 then
        return nil, "invalid token payload"
    end

    if type(claims.username) ~= "string" then
        return nil, "invalid token payload"
    end

    return { id = user_id, username = claims.username }
end

return auth
