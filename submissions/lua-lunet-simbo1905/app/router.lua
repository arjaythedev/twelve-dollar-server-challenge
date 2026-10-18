-- Router module: method+path dispatch with :param extraction.
--
-- lunet.jsonic wraps dkjson's encoder, so a table is encoded with the keyorder
-- given here and no other key order is guaranteed: the pairs fallback that
-- follows the keyorder is what dkjson does with anything not listed.

local json = require("lunet.jsonic")

local router = {}

-- Routes table - populated by route() function
local routes = {}

-- Add a route to the router
-- @param method: uppercase HTTP method (GET, POST, ...)
-- @param path: URL path pattern (e.g., "/posts/:id")
-- @param handler: function(request, params) returning status, body
function router.route(method, path, handler)
    local pattern_parts = {}
    for part in path:gmatch("[^/]+") do
        table.insert(pattern_parts, part)
    end
    table.insert(routes, {
        method = method,
        pattern = path,
        pattern_parts = pattern_parts,
        handler = handler
    })
end

-- Match a pre-split pattern against a path and extract parameters
-- @param pattern_parts: route pattern split on "/" (e.g., {"posts", ":id", "like"})
-- @param path: request path (e.g., "/posts/123/like")
-- @return params: table of parameters, or nil if no match
local function match_pattern(pattern_parts, path)
    local path_parts = {}
    for part in path:gmatch("[^/]+") do
        table.insert(path_parts, part)
    end

    if #pattern_parts ~= #path_parts then
        return nil
    end

    local params = {}
    for i, pp in ipairs(pattern_parts) do
        if pp:sub(1, 1) == ":" then
            params[pp:sub(2)] = path_parts[i]
        elseif pp ~= path_parts[i] then
            return nil
        end
    end

    return params
end

local function match(method, path)
    for _, route in ipairs(routes) do
        if route.method == method:upper() then
            local params = match_pattern(route.pattern_parts, path)
            if params then
                return route, params
            end
        end
    end
    return nil, nil
end

-- Dispatch a request: returns the status and the response body
function router.handle(request)
    local route, params = match(request.method, request.path)
    if not route then
        return 404, json.encode({ error = "not found" })
    end

    local ok, status, body = pcall(route.handler, request, params)
    if not ok then
        io.stderr:write("Handler error for ", request.method, " ", request.path, ": ", tostring(status), "\n")
        return 500, json.encode({ error = "internal server error" })
    end

    return status, body
end

return router
