-- Health: GET /health. A failed ping is a degraded response, not an
-- internal error.

local db = require("db")
local json = require("lunet.jsonic")
local router = require("lunet.http_router")

local STARTED_AT = os.time()

---GET /health: ping the database, degrade on failure.
---@param _ any The app config, passed by the router and ignored
---@param ctx table The request context
local function health(_, ctx)
    local ok, err = pcall(db.query_ro, "SELECT 1")
    if not ok then
        ctx.status = 503
        ctx.write('{"status":"degraded","db":"unreachable","error":' .. json.jstr(err) .. "}")
        return
    end
    ctx.status = 200
    ctx.write('{"status":"ok","db":"ok","uptime_s":' .. json.jint(os.time() - STARTED_AT) .. "}")
end

router.route("GET", "/health", health)
