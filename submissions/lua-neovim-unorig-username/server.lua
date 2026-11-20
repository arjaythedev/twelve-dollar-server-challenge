-- The whole server. Runs inside Neovim: nvim --headless --clean -l server.lua
local ffi, uv, api = require("ffi"), vim.loop, vim.api
local find, sub, match, lower, gsub, byte = string.find, string.sub, string.match, string.lower, string.gsub, string.byte

ffi.cdef [[
typedef struct sqlite3 sqlite3; typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char*, sqlite3**, int, const char*);
int sqlite3_exec(sqlite3*, const char*, void*, void*, char**);
int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_reset(sqlite3_stmt*);
int sqlite3_bind_int64(sqlite3_stmt*, int, int64_t);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void*);
const uint8_t* sqlite3_column_text(sqlite3_stmt*, int);
int sqlite3_column_bytes(sqlite3_stmt*, int);
int sqlite3_changes(sqlite3*);
const char* sqlite3_errmsg(sqlite3*);
const void* EVP_sha256(void);
void* HMAC(const void*, const char*, int, const char*, size_t, uint8_t*, unsigned*);
int EVP_DecodeBlock(uint8_t*, const char*, int);
]]
local S, K = ffi.load("libsqlite3.so.0"), ffi.load("libcrypto.so.3")
local ROW, DONE = 100, 101

local dbp = ffi.new("sqlite3*[1]")
assert(S.sqlite3_open_v2(os.getenv("SQLITE_PATH"), dbp, 2, nil) == 0, "cannot open SQLITE_PATH")
local db = dbp[0]
local function exec(sql) return S.sqlite3_exec(db, sql, nil, nil, nil) end
assert(exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000; PRAGMA busy_timeout=5000") == 0)
local function prep(sql)
  local p = ffi.new("sqlite3_stmt*[1]")
  assert(S.sqlite3_prepare_v2(db, sql, -1, p, nil) == 0, ffi.string(S.sqlite3_errmsg(db)))
  return p[0]
end
local POST = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
  .. " FROM posts p JOIN users u ON u.id = p.user_id "
local q_feed = prep(POST .. "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
local q_post = prep(POST .. "WHERE p.id = ?")
local q_ins = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
local q_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, id FROM posts WHERE id = ?2"
  .. " ON CONFLICT (user_id, post_id) DO NOTHING")
local q_exists, q_one = prep("SELECT 1 FROM posts WHERE id = ?"), prep("SELECT 1")
local q_begin, q_commit = prep("BEGIN IMMEDIATE"), prep("COMMIT")
local function run(q) -- steps a statement that returns no rows we keep
  local rc = S.sqlite3_step(q)
  S.sqlite3_reset(q)
  return rc
end

-- Output: a body is built in bb, then header + body are appended to ob (one Lua string per read).
local OB = 262144
local bb, ob, m, n = ffi.new("uint8_t[?]", 131072), ffi.new("uint8_t[?]", OB + 131072 + 128), 0, 0
local u8p = ffi.typeof("const uint8_t*")
local function put(s) ffi.copy(ob + n, s, #s); n = n + #s end
local function lit(s) ffi.copy(bb + m, s, #s); m = m + #s end
local function esc(p, len) -- JSON string contents
  for i = 0, len - 1 do
    local c = p[i]
    if c >= 32 and c ~= 34 and c ~= 92 then
      bb[m] = c; m = m + 1
    elseif c < 32 then
      lit(string.format("\\u%04x", c))
    else
      bb[m] = 92; bb[m + 1] = c; m = m + 2
    end
  end
end
local function col(q, i, f) f(S.sqlite3_column_text(q, i), S.sqlite3_column_bytes(q, i)) end
local function raw(p, len) ffi.copy(bb + m, p, len); m = m + len end
local function row(q)
  lit('{"id":'); col(q, 0, raw)
  lit(',"body":"'); col(q, 1, esc)
  lit('","created_at":"'); col(q, 2, esc)
  lit('","author":"'); col(q, 3, esc)
  lit('","like_count":'); col(q, 4, raw)
  lit("}")
end
local function head(status) return "HTTP/1.1 " .. status .. "\r\nContent-Type: application/json\r\nContent-Length: " end
local H200, H201 = head("200 OK"), head("201 Created")
local function send(h) -- header h + the body in bb
  put(h); put(tostring(m)); put("\r\n\r\n")
  ffi.copy(ob + n, bb, m); n = n + m
end
local function err(status, msg)
  local body = '{"error":"' .. msg .. '"}'
  return head(status) .. #body .. "\r\n\r\n" .. body
end
local E404, E_ID, E_POST = err("404 Not Found", "not found"), err("400 Bad Request", "invalid post id"), err("404 Not Found", "post not found")
local E_BEARER, E_TOKEN = err("401 Unauthorized", "missing bearer token"), err("401 Unauthorized", "invalid or expired token")
local E_PAYLOAD, E_JSON = err("401 Unauthorized", "invalid token payload"), err("400 Bad Request", "malformed JSON body")
local E_BODY, E_LONG = err("400 Bad Request", "body is required"), err("400 Bad Request", "body must be at most 500 characters")
local E500 = err("500 Internal Server Error", "internal server error")

local function post_id(s) -- a positive integer, or nil; one too long for SQLite becomes an id no row has
  local id = find(s, "^%d+$") and (#s < 19 and tonumber(s) or 2 ^ 62)
  return id and id > 0 and id or nil
end

local t0, hits = os.time(), 0
local function get(buf, pos)
  m = 0
  if find(buf, "^GET /feed[ ?]", pos) then
    lit('{"posts":[')
    local rc = S.sqlite3_step(q_feed)
    while rc == ROW do
      if m > 10 then lit(",") end
      row(q_feed)
      rc = S.sqlite3_step(q_feed)
    end
    S.sqlite3_reset(q_feed)
    if rc ~= DONE then return put(E500) end
    lit("]}")
    return send(H200)
  end
  local id = match(buf, "^GET /posts/([^/ ?]+)[ ?]", pos)
  if id then
    id = post_id(id)
    if not id then return put(E_ID) end
    S.sqlite3_bind_int64(q_post, 1, id)
    local rc = S.sqlite3_step(q_post)
    if rc == ROW then lit('{"post":'); row(q_post); lit("}") end
    S.sqlite3_reset(q_post)
    if rc == ROW then return send(H200) end
    return put(rc == DONE and E_POST or E500)
  end
  if not find(buf, "^GET /health[ ?]", pos) then return put(E404) end
  if run(q_one) == ROW then
    lit('{"status":"ok","db":"ok","uptime_s":' .. os.time() - t0 .. "}")
    return send(H200)
  end
  lit('{"status":"degraded","db":"unreachable","error":"')
  local e = ffi.string(S.sqlite3_errmsg(db))
  esc(ffi.cast(u8p, e), #e)
  lit('"}')
  send(head("503 Service Unavailable"))
end

local secret, sha, md, tmp = os.getenv("JWT_SECRET"), K.EVP_sha256(), ffi.new("uint8_t[32]"), ffi.new("uint8_t[16384]")
local function unb64(s) -- base64url -> string, or nil
  local pad = -#s % 4
  if pad == 3 or #s > 16000 or find(s, "[^%w_-]") then return end
  s = gsub(s, "[-_]", { ["-"] = "+", _ = "/" }) .. string.rep("=", pad)
  local len = K.EVP_DecodeBlock(tmp, s, #s)
  if len >= pad then return ffi.string(tmp, len - pad) end
end
local function json(s) -- decoded value, or nil
  local ok, v = pcall(vim.json.decode, s or "")
  if ok then return v end
end
local function auth(tok) -- user id and username, or nil and the error response
  if not tok then return nil, E_BEARER end
  local h, p, s = match(tok, "^([^.]+)%.([^.]+)%.([^.]+)$")
  local hd = h and json(unb64(h))
  if type(hd) ~= "table" or hd.alg ~= "HS256" then return nil, E_TOKEN end
  K.HMAC(sha, secret, #secret, tok, #h + 1 + #p, md, nil)
  local pl = unb64(s) == ffi.string(md, 32) and json(unb64(p))
  if type(pl) ~= "table" or type(pl.exp) ~= "number" or pl.exp <= os.time() then return nil, E_TOKEN end
  if pl.nbf ~= nil and (type(pl.nbf) ~= "number" or pl.nbf > os.time()) then return nil, E_TOKEN end
  local uid = type(pl.sub) == "string" and #pl.sub < 19 and post_id(pl.sub)
  if not uid or type(pl.username) ~= "string" then return nil, E_PAYLOAD end
  return uid, pl.username
end

local function num(t) -- a JSON number
  local i = match(t, "^%-?0()") or match(t, "^%-?[1-9]%d*()")
  if not i then return false end
  i = match(t, "^%.%d+()", i) or i
  return (match(t, "^[eE][+-]?%d+()", i) or i) > #t
end
-- vim.json also takes NaN, Infinity, hex, 01, +1, 1. and raw control characters in strings. JSON does not.
local function strict(s)
  s = gsub(gsub(s, "\\.", ""), '"[^"%z\1-\31]*"', "")
  if find(s, '"', 1, true) then return false end
  for t in string.gmatch(s, "[%w.+-]+") do
    if t ~= "true" and t ~= "false" and t ~= "null" and not num(t) then return false end
  end
  return true
end

local dirty = false -- a write transaction is open; committed before anything from this tick is sent
local function write(buf, pos, h, body)
  local id
  if not find(buf, "^POST /posts[ ?]", pos) then
    id = match(buf, "^POST /posts/([^/ ?]+)/like[ ?]", pos)
    if not id then return put(E404) end
  end
  local _, e = find(h, "\nauthorization:[ \t]*")
  local uid, name = auth(e and match(buf, "^Bearer ([^\r]*)", pos + e))
  if not uid then return put(name) end
  local text
  if id then
    id = post_id(id)
    if not id then return put(E_ID) end
  else
    local ok, v = pcall(vim.json.decode, body)
    if not ok or not strict(body) then return put(E_JSON) end
    text = type(v) == "table" and type(v.body) == "string" and match(v.body, "^%s*(.*%S)")
    if not text then return put(E_BODY) end
    local _, cont = gsub(text, "[\128-\191]", "")
    if #text - cont > 500 then return put(E_LONG) end
  end
  if not dirty then
    if run(q_begin) ~= DONE then return put(E500) end
    dirty = true
  end
  m = 0
  if id then
    S.sqlite3_bind_int64(q_like, 1, uid)
    S.sqlite3_bind_int64(q_like, 2, id)
    if run(q_like) ~= DONE then return put(E500) end
    local new = S.sqlite3_changes(db) == 1
    if not new then
      S.sqlite3_bind_int64(q_exists, 1, id)
      local rc = run(q_exists)
      if rc ~= ROW then return put(rc == DONE and E_POST or E500) end
    end
    lit('{"liked":true,"already_liked":' .. tostring(not new) .. ',"post_id":' .. string.format("%d", id) .. "}")
    return send(new and H201 or H200)
  end
  S.sqlite3_bind_int64(q_ins, 1, uid)
  S.sqlite3_bind_text(q_ins, 2, text, #text, nil)
  local rc = S.sqlite3_step(q_ins)
  if rc == ROW then
    lit('{"post":{"id":'); col(q_ins, 0, raw)
    lit(',"body":"'); esc(ffi.cast(u8p, text), #text)
    lit('","created_at":"'); col(q_ins, 1, esc)
    lit('","author":"'); esc(ffi.cast(u8p, name), #name)
    lit('","like_count":0}}')
    rc = S.sqlite3_step(q_ins)
  end
  S.sqlite3_reset(q_ins)
  if rc ~= DONE then return put(E500) end
  send(H201)
end

local queue, nq = {}, 0 -- connections with output waiting for the end of this loop tick
local srv, conns = uv.new_tcp(), 0
local function close(c)
  if not c.closed then
    c.closed = true; conns = conns - 1
    uv.close(c.h)
    -- A child that handed the socket on and drained has nothing left to do. The first process stays: it is the service.
    if conns == 0 and not srv and arg[1] then os.exit(0) end
  end
end
local function out(c) -- moves ob into the connection's pending output
  c.out = (c.out or "") .. ffi.string(ob, n)
  n = 0
  if not c.queued then
    c.queued = true
    nq = nq + 1; queue[nq] = c
  end
end
-- shortcut: no Transfer-Encoding: chunked, no Expect: 100-continue, GET bodies are not skipped,
-- Connection: close is left to the client. Add them if a client needs them.
local function on_data(c, chunk)
  local buf = c.buf and c.buf .. chunk or chunk
  local pos, len = 1, #buf
  while pos <= len do
    local _, he = find(buf, "\r\n\r\n", pos, true)
    if not he then break end
    local nxt = he + 1
    if byte(buf, pos) == 71 then -- "G"
      get(buf, pos)
    else
      local h = lower(sub(buf, pos, he))
      nxt = nxt + (tonumber(match(h, "\ncontent%-length:[ \t]*(%d+)")) or 0)
      if nxt > len + 1 then break end
      write(buf, pos, h, sub(buf, he + 1, nxt - 1))
    end
    pos, hits = nxt, hits + 1
    if n > OB then out(c) end
  end
  if n > 0 then out(c) end
  if len - pos >= 1048576 then return close(c) end -- a request this large is not one of ours
  c.buf = pos <= len and sub(buf, pos) or nil
end

local function flush() -- group commit, then one write per connection
  local ok = true
  if dirty then
    dirty = false
    ok = run(q_commit) == DONE
    if not ok then exec("ROLLBACK") end
  end
  for i = 1, nq do
    local c = queue[i]
    local s = c.out
    queue[i], c.out, c.queued = nil, nil, nil
    if not ok then -- nothing was committed, so nobody in this tick gets an answer
      close(c)
    elseif not c.closed then
      local w = uv.try_write(c.h, s) or 0
      if w < #s then uv.write(c.h, sub(s, w + 1)) end
    end
  end
  nq = 0
end

-- One process holds at most 60000 connections (LimitNOFILE is 65535). At that point a fresh Neovim
-- inherits the listening socket as fd 3 (arg[1] is set) and this one only keeps what it has.
local nvim = vim.v.progpath
if arg[1] then assert(uv.tcp_open(srv, 3)) else assert(uv.tcp_bind(srv, os.getenv("HOST"), tonumber(os.getenv("PORT")))) end
assert(uv.listen(srv, 4096, function(e)
  local h = uv.new_tcp()
  if e or not uv.accept(srv, h) then return uv.close(h) end
  uv.tcp_nodelay(h, true)
  local c = { h = h }
  conns = conns + 1
  uv.read_start(h, function(_, chunk)
    if chunk then on_data(c, chunk) else close(c) end
  end)
  if conns >= 60000 and uv.spawn(nvim, { args = { "--headless", "--clean", "-l", arg[0], "3" }, stdio = { nil, 1, 2, srv } }) then
    uv.close(srv)
    srv = nil
  end
end))

-- Group commit and the writes happen once per turn of the editor's event loop, right after its I/O.
uv.check_start(uv.new_check(), function()
  if nq > 0 then flush() end
end)
-- Buffer 1 shows live counters. Read them from outside with:
-- nvim --server <the socket under ${XDG_RUNTIME_DIR:-/tmp/nvim.$USER}> --remote-expr 'getline(1)'
uv.timer_start(uv.new_timer(), 1000, 1000, vim.schedule_wrap(function()
  api.nvim_buf_set_lines(0, 0, -1, false, { ("uptime %d s, %d connections, %d requests"):format(os.time() - t0, conns, hits) })
end))
while true do vim.wait(2 ^ 30) end -- the editor's own event loop, forever
