-- Fixed-shape JSON encode helpers: the response shapes are known, so the
-- byte strings are assembled with static prefixes and one escaper instead of
-- walking tables. The helpers are the upstream jsonic surface since the
-- v0.14.1 release (#203); error_body stays the singular SPEC shape.

local json = require("lunet.jsonic")

local render = {}

function render.jstr(value)
    return json.jstr(value)
end

function render.jint(value)
    return json.jint(value)
end

function render.error_body(message)
    return '{"error":' .. json.jstr(message) .. "}"
end

return render
