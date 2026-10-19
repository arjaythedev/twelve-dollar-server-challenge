-- Feed API - the $12 Server Challenge submission
-- Built with lunet (libuv + LuaJIT coroutine runtime)

package.path = "./app/?.lua;./?.lua;" .. package.path

io.stdout:setvbuf("no")

local lunet = require("lunet")
local socket = require("lunet.socket")
local http = require("lunet.http")
local http_context = require("lunet.http_context")
local router = require("lunet.http_router")

local json = require("lunet.jsonic")
local config = require("config")
local db = require("db")

-- Route modules register themselves on require.
require("auth")
require("posts")
require("health")

-- Warm up jsonic's native decoder before the event loop starts: a lazy FFI
-- load under live worker threads can deadlock (examples/08 guidance).
json.decode("{}")

---Serve one accepted connection: read requests, dispatch, write responses,
---until the client closes or the request stream breaks.
---@param client userdata The accepted socket handle
local function handle_client(client)
    while true do
        local request, parse_err, parse_status = http.read_request(function()
            return socket.read(client)
        end)
        if not request and not parse_status then
            -- EOF before any bytes: a closed or idle client owns the close
            socket.close(client)
            return
        end
        if not request then
            -- Broken framing: the connection state is untrustworthy, close
            -- after the one response.
            socket.write(client, http.json_response(parse_status or 400, { error = parse_err }, nil, false))
            socket.close(client)
            return
        end

        local declared_close = request.headers["connection"] == "close"
        local persistent = not declared_close

        local ctx = http_context.new(request)
        ctx.env_config = config

        -- The SPEC's byte shape for an unmatched path; the router's own
        -- built-in 404 body is its JSON shape, not the SPEC's.
        if not router.match(ctx.method, ctx.path) then
            socket.write(client, http.response(404, nil, '{"error":"not found"}', nil, persistent))
            if declared_close then socket.close(client) return end
        else
            router.handle(ctx)
            socket.write(client, http.response(ctx.status, ctx.res_headers, ctx.response_body(), nil, persistent))
            if declared_close then socket.close(client) return end
        end
    end
end

lunet.spawn(function()
    local _, open_err = db.connect(config.SQLITE_PATH)
    if open_err then
        print("Failed to open " .. config.SQLITE_PATH .. ": " .. tostring(open_err))
        os.exit(1)
    end

    local listener, listen_err = socket.listen("tcp", config.HOST, config.PORT)
    if not listener then
        print("Failed to listen: " .. tostring(listen_err))
        os.exit(1)
    end

    print("Feed API listening on tcp://" .. config.HOST .. ":" .. config.PORT)

    while true do
        local client = socket.accept(listener)
        if client then
            lunet.spawn(function()
                handle_client(client)
            end)
        end
    end
end)
