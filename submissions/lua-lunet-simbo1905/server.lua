-- Feed API - the $12 Server Challenge submission
-- Built with lunet (libuv + LuaJIT coroutine runtime)

package.path = "./app/?.lua;./lib/?.lua;./?.lua;" .. package.path

io.stdout:setvbuf("no")

local lunet = require("lunet")
local socket = require("lunet.socket")
local http = require("http")

local config = require("config")
local db = require("db")
local router = require("router")
require("routes")

local function handle_request(request, persistent)
    local status, body = router.handle(request)
    return http.response(status, nil, body, persistent)
end

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
            socket.write(client, http.json_response(parse_status or 400, { error = parse_err }))
            socket.close(client)
            return
        end

        local response = handle_request(request, true)

        socket.write(client, response)
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
