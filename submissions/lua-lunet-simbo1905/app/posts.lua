-- Posts: the feed model and the /feed, /posts routes registered on
-- lunet.http_router (a route module registers itself when required).
-- Handlers drive the per-request context directly (ctx.status + ctx.write)
-- so the response bytes stay exactly the fixed shapes the byte-exact
-- rendering below builds.

local db = require("db")
local json = require("lunet.jsonic")
local render = require("render")
local auth = require("auth")
local router = require("lunet.http_router")

local jstr, jint = render.jstr, render.jint
local error_body = render.error_body

local POST_SELECT = [[
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
]]

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

local function parse_post_id(raw)
    local id = raw:match("^%d+$") and tonumber(raw)
    if not id or id < 1 then
        return nil
    end
    return id
end

local function feed(_, ctx)
    local rows = db.query(POST_SELECT .. " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    local parts = {}
    for i, row in ipairs(rows) do
        parts[i] = post_object_json(row)
    end
    ctx.status = 200
    ctx.write('{"posts":[' .. table.concat(parts, ",") .. "]}")
end

local function show_post(_, ctx, params)
    local id = parse_post_id(params.id)
    if not id then
        ctx.status = 400
        ctx.write(error_body("invalid post id"))
        return
    end

    local rows = db.query(POST_SELECT .. " WHERE p.id = ?", id)
    if not rows[1] then
        ctx.status = 404
        ctx.write(error_body("post not found"))
        return
    end

    ctx.status = 200
    ctx.write('{"post":' .. post_object_json(rows[1]) .. "}")
end

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

    local existing = db.query("SELECT 1 FROM posts WHERE id = ?", id)
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
    ctx.write('{"liked":true,"already_liked":' .. tostring(already_liked) .. ',"post_id":' .. jint(id) .. "}")
end

router.route("GET", "/feed", feed)
router.route("GET", "/posts/:id", show_post)
router.route("POST", "/posts", create_post)
router.route("POST", "/posts/:id/like", like_post)
