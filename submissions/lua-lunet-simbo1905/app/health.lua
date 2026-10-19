-- Health: GET /health. A failed ping is the degraded response, not an
-- internal error.

local db = require("db")
local render = require("render")
local router = require("lunet.http_router")

local jint, jstr = render.jint, render.jstr

local STARTED_AT = os.time()

local function health(_, ctx)
    local ok, err = pcall(db.query, "SELECT 1")
    if not ok then
        ctx.status = 503
        ctx.write('{"status":"degraded","db":"unreachable","error":' .. jstr(err) .. "}")
        return
    end
    ctx.status = 200
    ctx.write('{"status":"ok","db":"ok","uptime_s":' .. jint(os.time() - STARTED_AT) .. "}")
end

router.route("GET", "/health", health)
