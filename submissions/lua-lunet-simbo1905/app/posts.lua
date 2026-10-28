-- Posts: the feed model and the /feed, /posts routes registered on
-- lunet.http_router (a route module registers itself when required).

local db = require("db")
local json = require("lunet.jsonic")
local auth = require("auth")
local router = require("lunet.http_router")

-- The single spec error shape.
---@param message string The error message; jstr-quoted into the fixed shape
---@return string body The spec error body
local function error_body(message)
    return '{"error":' .. json.jstr(message) .. "}"
end

local POST_SELECT = [[
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
]]

---Render one post object in the spec's byte shape.
---@param id integer Post id
---@param body string Post body text (unprocessed)
---@param created_at string Wire-format timestamp
---@param author string Author username
---@param like_count integer Like count
---@return string json The rendered post object
local function post_json(id, body, created_at, author, like_count)
    return '{"id":' .. json.jint(id)
        .. ',"body":' .. json.jstr(body)
        .. ',"created_at":' .. json.jstr(created_at)
        .. ',"author":' .. json.jstr(author)
        .. ',"like_count":' .. json.jint(like_count) .. "}"
end

---Render a SELECT row as the spec's post object.
---@param row table One row of POST_SELECT: id, body, created_at, username, like_count
---@return string json The rendered post object
local function post_object_json(row)
    return post_json(row.id, row.body, row.created_at, row.username, row.like_count)
end

---Parse a route's :id capture.
---@param raw string The captured id segment
---@return integer|nil id Positive integer, nil and no reason when invalid
local function parse_post_id(raw)
    local id = raw:match("^%d+$") and tonumber(raw)
    if not id or id < 1 then
        return nil
    end
    return id
end

---GET /feed: the 20 newest posts.
---@param _ any The app config, passed by the router and ignored
---@param ctx table The request context
local function feed(_, ctx)
    local rows = db.query_ro(POST_SELECT .. " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    local parts = {}
    for i, row in ipairs(rows) do
        parts[i] = post_object_json(row)
    end
    ctx.status = 200
    ctx.write('{"posts":[' .. table.concat(parts, ",") .. "]}")
end

---GET /posts/:id.
---@param _ any The app config, passed by the router and ignored
---@param ctx table The request context
---@param params table Route captures: params.id
local function show_post(_, ctx, params)
    local id = parse_post_id(params.id)
    if not id then
        ctx.status = 400
        ctx.write(error_body("invalid post id"))
        return
    end

    local rows = db.query_ro(POST_SELECT .. " WHERE p.id = ?", id)
    if not rows[1] then
        ctx.status = 404
        ctx.write(error_body("post not found"))
        return
    end

    ctx.status = 200
    ctx.write('{"post":' .. post_object_json(rows[1]) .. "}")
end

---POST /posts (auth).
---@param _ any The app config, passed by the router and ignored
---@param ctx table The request context
local function create_post(_, ctx)
    local user, auth_error = auth.authenticate(ctx)
    if not user then
        ctx.status = 401
        ctx.write(error_body(auth_error))
        return
    end

    local payload = json.decode(ctx.body)
    if type(payload) ~= "table" then
        ctx.status = 400
        ctx.write(error_body("malformed JSON body"))
        return
    end

    local body = payload.body
    if type(body) ~= "string" then
        ctx.status = 400
        ctx.write(error_body("body is required"))
        return
    end

    body = body:match("^%s*(.-)%s*$")
    if body == "" then
        ctx.status = 400
        ctx.write(error_body("body is required"))
        return
    end
    if #body > 500 then
        ctx.status = 400
        ctx.write(error_body("body must be at most 500 characters"))
        return
    end

    local rows = db.query("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", user.id, body)
    ctx.status = 201
    ctx.write('{"post":' .. post_json(rows[1].id, body, rows[1].created_at, user.username, 0) .. "}")
end

---POST /posts/:id/like (auth): one like per (user, post).
---@param _ any The app config, passed by the router and ignored
---@param ctx table The request context
---@param params table Route captures: params.id
local function like_post(_, ctx, params)
    local user, auth_error = auth.authenticate(ctx)
    if not user then
        ctx.status = 401
        ctx.write(error_body(auth_error))
        return
    end

    local id = parse_post_id(params.id)
    if not id then
        ctx.status = 400
        ctx.write(error_body("invalid post id"))
        return
    end

    local existing = db.query_ro("SELECT 1 FROM posts WHERE id = ?", id)
    if not existing[1] then
        ctx.status = 404
        ctx.write(error_body("post not found"))
        return
    end

    local result = db.exec(
        "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
        user.id, id)
    local already_liked = result.affected_rows == 0
    ctx.status = already_liked and 200 or 201
    ctx.write('{"liked":true,"already_liked":' .. tostring(already_liked) .. ',"post_id":' .. json.jint(id) .. "}")
end

router.route("GET", "/feed", feed)
router.route("GET", "/posts/:id", show_post)
router.route("POST", "/posts", create_post)
router.route("POST", "/posts/:id/like", like_post)
