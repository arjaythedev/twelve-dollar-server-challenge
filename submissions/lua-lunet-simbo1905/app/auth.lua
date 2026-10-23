-- Bearer-token authentication over lunet.security's HS256 decoder.

local config = require("config")
local security = require("lunet.security")

local auth = {}

---Verify the context's bearer token against JWT_SECRET.
---@param ctx table Request context; reads ctx.headers["authorization"]
---@return table|nil owner The token owner as { id = integer, username = string }, nil when rejected
---@return string|nil reason The rejection reason for error_body:
---"missing bearer token", "invalid or expired token", or "invalid token payload"
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
