local config = require("config")
local db = require("db")
local jwt = require("jwt")
local json = require("lunet.jsonic")
local router = require("router")

-- Fixed-shape JSON encode: the response shapes are known, so the byte strings are
-- assembled with static prefixes and one escaper instead of walking tables. The
-- escaper is jsonic's own quotestring (vendored dkjson 2.10), so escaping matches
-- the old encode byte for byte; numbers and booleans are integers/literals here.

local POST_SELECT = [[
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
]]

local STARTED_AT = os.time()

local function jstr(value)
    if type(value) ~= "string" then
        return json.encode(value)
    end
    return json.quotestring(value)
end

local function jint(value)
    return tostring(value)
end

local function post_json(id, body, created_at, author, like_count)
    return '{"id":' .. jint(id)
        .. ',"body":' .. jstr(body)
        .. ',"created_at":' .. jstr(created_at)
        .. ',"author":' .. jstr(author)
        .. ',"like_count":' .. jint(like_count) .. "}"
end

local function post_object_json(row)
    return post_json(row.id, row.body, row.created_at, row.username, row.like_count)
end

local function error_body(message)
    return '{"error":' .. jstr(message) .. "}"
end

local function parse_post_id(raw)
    local id = raw:match("^%d+$") and tonumber(raw)
    if not id or id < 1 then
        return nil
    end
    return id
end

local function authenticate(request)
    local authorization = request.headers["authorization"]
    if not authorization then
        return nil, "missing bearer token"
    end

    local token = authorization:match("^Bearer%s+([^%s]+)$")
    if not token then
        return nil, "missing bearer token"
    end

    local payload = jwt.decode(token, config.JWT_SECRET)
    if not payload then
        return nil, "invalid or expired token"
    end

    local user_id = type(payload.sub) == "string" and payload.sub:match("^%d+$") and tonumber(payload.sub)
    if not user_id or user_id < 1 then
        return nil, "invalid token payload"
    end

    if type(payload.username) ~= "string" then
        return nil, "invalid token payload"
    end

    return { id = user_id, username = payload.username }
end

local function health()
    -- A failed ping is the degraded response, not an internal error
    local ok, err = pcall(db.query, "SELECT 1")
    if not ok then
        return 503, '{"status":"degraded","db":"unreachable","error":' .. jstr(err) .. "}"
    end
    return 200, '{"status":"ok","db":"ok","uptime_s":' .. jint(os.time() - STARTED_AT) .. "}"
end

local function feed()
    local rows = db.query(POST_SELECT .. " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    local parts = {}
    for i, row in ipairs(rows) do
        parts[i] = post_object_json(row)
    end
    return 200, '{"posts":[' .. table.concat(parts, ",") .. "]}"
end

local function show_post(request, params)
    local id = parse_post_id(params.id)
    if not id then
        return 400, error_body("invalid post id")
    end

    local rows = db.query(POST_SELECT .. " WHERE p.id = ?", id)
    if not rows[1] then
        return 404, error_body("post not found")
    end

    return 200, '{"post":' .. post_object_json(rows[1]) .. "}"
end

local function create_post(request)
    local user, auth_error = authenticate(request)
    if not user then
        return 401, error_body(auth_error)
    end

    local payload = json.decode(request.body)
    if type(payload) ~= "table" then
        return 400, error_body("malformed JSON body")
    end

    local body = payload.body
    if type(body) ~= "string" then
        return 400, error_body("body is required")
    end

    body = body:match("^%s*(.-)%s*$")
    if body == "" then
        return 400, error_body("body is required")
    end
    if #body > 500 then
        return 400, error_body("body must be at most 500 characters")
    end

    local rows = db.query("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", user.id, body)
    return 201, '{"post":' .. post_json(rows[1].id, body, rows[1].created_at, user.username, 0) .. "}"
end

local function like_post(request, params)
    local user, auth_error = authenticate(request)
    if not user then
        return 401, error_body(auth_error)
    end

    local id = parse_post_id(params.id)
    if not id then
        return 400, error_body("invalid post id")
    end

    local existing = db.query("SELECT 1 FROM posts WHERE id = ?", id)
    if not existing[1] then
        return 404, error_body("post not found")
    end

    local result = db.exec("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING", user.id, id)
    local already_liked = result.affected_rows == 0
    return already_liked and 200 or 201,
        '{"liked":true,"already_liked":' .. tostring(already_liked) .. ',"post_id":' .. jint(id) .. "}"
end

router.route("GET", "/health", health)
router.route("GET", "/feed", feed)
router.route("GET", "/posts/:id", show_post)
router.route("POST", "/posts", create_post)
router.route("POST", "/posts/:id/like", like_post)
