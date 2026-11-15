-- server.lua: loaded by server.tex. Plain Lua 5.3 inside LuaTeX; TeX's main loop calls tick() forever.
local mime = require("mime")
local ev = assert(package.loadlib("./bin/evio.so", "luaopen_evio"))() -- epoll, see evio.c
local sqlite3 = assert(package.loadlib("./bin/lsqlite3.so", "luaopen_lsqlite3"))()
local find, sub, match, lower, gsub, byte, char = string.find, string.sub, string.match, string.lower, string.gsub, string.byte, string.char
local concat, digest = table.concat, sha2.digest256
local F = lua.get_functions_table()
local ROW, DONE = sqlite3.ROW, sqlite3.DONE

-- SQLite: one connection, prepared statements reused. Post objects are written by json_object.
local db = assert(sqlite3.open(os.getenv("SQLITE_PATH")), "cannot open SQLITE_PATH")
assert(db:exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;"
  .. " PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000") == sqlite3.OK, db:errmsg())
local function prep(sql) return assert(db:prepare(sql), db:errmsg()) end
local OBJ = "SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,"
  .. " 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)) FROM posts p JOIN users u ON u.id = p.user_id "
local q_feed = prep(OBJ .. "ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
local q_post = prep(OBJ .. "WHERE p.id = ?")
local q_ins = prep("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING json_object('id', id, 'body', body,"
  .. " 'created_at', created_at, 'author', ?3, 'like_count', 0)")
local q_like = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, id FROM posts WHERE id = ?2"
  .. " ON CONFLICT (user_id, post_id) DO NOTHING")
local q_exists, q_one = prep("SELECT 1 FROM posts WHERE id = ?"), prep("SELECT 1")
local q_begin, q_commit = prep("BEGIN IMMEDIATE"), prep("COMMIT")
local function run(q) local rc = q:step(); q:reset(); return rc end

-- Responses. reply() queues one: the connection, the status, and the TeX that expands to its body.
-- At the end of a tick all of it goes to TeX in one tex.sprint; TeX expands each body into \out and
-- calls \luafunction2, which takes it, so bodies come back in request order.
local tex_code, jc, js, ji, nj, taken = {}, {}, {}, {}, 0, 0
local function reply(c, status, macro, inner)
  nj = nj + 1
  jc[nj], js[nj], ji[nj] = c, status, inner or false
  tex_code[nj] = "\\xdef\\out\1" .. macro .. "\2\\luafunction2 "
end
local function oops(c, status, msg) reply(c, status, "\\oops\1" .. msg .. "\2") end
local B400, B401, B404 = "400 Bad Request", "401 Unauthorized", "404 Not Found"

local function jstr(s) -- a JSON string literal
  return '"' .. gsub(s, '[%c"\\]', function(ch)
    return ch == '"' and '\\"' or ch == "\\" and "\\\\" or string.format("\\u%04x", byte(ch))
  end) .. '"'
end

-- Strict JSON (RFC 8259): returns true and the value, or false. Objects are tables, arrays carry ARR.
local NULL, ARR, BAD = {}, {}, {}
local LIT = { ["true"] = true, ["false"] = false, null = NULL }
local ESC = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
local function decode(s)
  local pos = 1
  local function fail() error(BAD, 0) end
  local function ws() pos = match(s, "^[ \t\r\n]*()", pos) end
  local function str() -- pos is at the opening quote
    local out, i = {}, pos + 1
    while true do
      local _, b = find(s, '^[^"\\\0-\31]*', i)
      out[#out + 1] = sub(s, i, b)
      local ch = byte(s, b + 1)
      if ch == 34 then pos = b + 2; return concat(out) end
      if ch ~= 92 then fail() end -- a raw control character, or the end of the input
      local e = sub(s, b + 2, b + 2)
      if ESC[e] then
        out[#out + 1], i = ESC[e], b + 3
      elseif e == "u" then
        local cp = tonumber(match(s, "^%x%x%x%x", b + 3) or fail(), 16)
        i = b + 7
        local lo = cp >= 0xD800 and cp < 0xDC00 and match(s, "^\\u(%x%x%x%x)", i)
        lo = lo and tonumber(lo, 16)
        if lo and lo >= 0xDC00 and lo < 0xE000 then cp, i = 0x10000 + (cp - 0xD800) * 0x400 + lo - 0xDC00, i + 6 end
        out[#out + 1] = utf8.char(cp)
      else
        fail()
      end
    end
  end
  local value
  local function members(close, t, d, item) -- the rest of an object or array after its opening bracket
    ws()
    if byte(s, pos) == close then pos = pos + 1; return t end
    repeat
      ws(); item(t, d); ws()
      local ch = byte(s, pos); pos = pos + 1
      if ch == close then return t end
    until ch ~= 44
    fail()
  end
  local function member(t, d)
    if byte(s, pos) ~= 34 then fail() end
    local k = str(); ws()
    if byte(s, pos) ~= 58 then fail() end
    pos = pos + 1
    t[k] = value(d)
  end
  local function element(t, d) t[#t + 1] = value(d) end
  function value(d)
    if d > 64 then fail() end
    ws()
    local ch = byte(s, pos)
    if ch == 123 then pos = pos + 1; return members(125, {}, d + 1, member) end
    if ch == 91 then pos = pos + 1; return members(93, { [ARR] = true }, d + 1, element) end
    if ch == 34 then return str() end
    for word, v in pairs(LIT) do
      if sub(s, pos, pos + #word - 1) == word then pos = pos + #word; return v end
    end
    local i = match(s, "^%-?0()", pos) or match(s, "^%-?[1-9]%d*()", pos) or fail()
    i = match(s, "^%.%d+()", i) or i
    i = match(s, "^[eE][+-]?%d+()", i) or i
    local n = tonumber(sub(s, pos, i - 1)); pos = i
    return n
  end
  if not s then return false end
  local ok, v = pcall(value, 0)
  if not ok then return false end
  ws()
  return pos > #s, v
end
local function object(ok, v) return ok and type(v) == "table" and v ~= NULL and not v[ARR] and v end

-- HS256: HMAC-SHA256 (RFC 2104) over LuaTeX's built-in sha2, pads built once from JWT_SECRET.
local key = os.getenv("JWT_SECRET") or ""
if #key > 64 then key = digest(key) end
key = key .. string.rep("\0", 64 - #key)
local ipad = gsub(key, ".", function(ch) return char(byte(ch) ~ 0x36) end)
local opad = gsub(key, ".", function(ch) return char(byte(ch) ~ 0x5c) end)
local URL = { ["+"] = "-", ["/"] = "_", ["="] = "" }
local function unb64(s) -- base64url -> bytes, or nil
  if #s % 4 == 1 or find(s, "[^%w_-]") then return nil end
  return (mime.unb64(gsub(s, "[-_]", { ["-"] = "+", _ = "/" }) .. string.rep("=", -#s % 4)))
end

local function pos_int(s) -- a positive integer, or nil; one too long for SQLite becomes an id no row has
  if not find(s, "^%d+$") then return nil end
  s = match(s, "^0*(.*)") -- leading zeros first, so 0000000000000000001 is still 1
  local n = #s < 19 and tonumber(s) or #s > 0 and math.maxinteger
  return n and n > 0 and n or nil
end

local function auth(v) -- user id and username, or nil and an error message
  if not v or sub(v, 1, 7) ~= "Bearer " then return nil, "missing bearer token" end
  local h, p, s = match(v, "^Bearer ([%w_-]+)%.([%w_-]+)%.([%w_-]+)%s*$")
  local hd = h and object(decode(unb64(h)))
  if not hd or hd.alg ~= "HS256" then return nil, "invalid or expired token" end
  local mac = gsub(mime.b64(digest(opad .. digest(ipad .. h .. "." .. p))), "[+/=]", URL)
  local pl = digest(mac) == digest(s) and object(decode(unb64(p))) -- compare hashes: no timing hint about mac
  local now = os.time()
  if not pl or type(pl.exp) ~= "number" or pl.exp <= now then return nil, "invalid or expired token" end
  if pl.nbf ~= nil and (type(pl.nbf) ~= "number" or pl.nbf > now) then return nil, "invalid or expired token" end
  local uid = type(pl.sub) == "string" and pos_int(pl.sub)
  if not uid or type(pl.username) ~= "string" then return nil, "invalid token payload" end
  return uid, pl.username
end

local t0 = os.time()
local function get(c, buf, pos)
  if find(buf, "^GET /feed[ ?]", pos) then
    local rows, n, rc = {}, 0, q_feed:step()
    while rc == ROW do
      n = n + 1; rows[n] = q_feed:get_value(0)
      rc = q_feed:step()
    end
    q_feed:reset()
    if rc ~= DONE then error(db:errmsg()) end
    return reply(c, "200 OK", "\\feed", concat(rows, ","))
  end
  local id = match(buf, "^GET /posts/([^/ ?]+)[ ?]", pos)
  if id then
    id = pos_int(id)
    if not id then return oops(c, B400, "invalid post id") end
    q_post:bind(1, id)
    local rc = q_post:step()
    local o = rc == ROW and q_post:get_value(0)
    q_post:reset()
    if o then return reply(c, "200 OK", "\\post", o) end
    if rc ~= DONE then error(db:errmsg()) end
    return oops(c, B404, "post not found")
  end
  if not find(buf, "^GET /health[ ?]", pos) then return oops(c, B404, "not found") end
  if run(q_one) == ROW then return reply(c, "200 OK", "\\health\1" .. os.time() - t0 .. "\2") end
  reply(c, "503 Service Unavailable", "\\degraded", jstr(db:errmsg()))
end

local dirty = false -- a write transaction is open; it commits before anything from this tick is sent
local function write(c, buf, pos, h, body)
  local id
  if not find(buf, "^POST /posts[ ?]", pos) then
    id = match(buf, "^POST /posts/([^/ ?]+)/like[ ?]", pos)
    if not id then return oops(c, B404, "not found") end
  end
  local _, e = find(h, "\nauthorization:[ \t]*")
  local uid, name = auth(e and match(buf, "^[^\r]*", pos + e))
  if not uid then return oops(c, B401, name) end
  local text
  if id then
    id = pos_int(id)
    if not id then return oops(c, B400, "invalid post id") end
  else
    local ok, v = decode(body)
    if not ok then return oops(c, B400, "malformed JSON body") end
    v = object(ok, v)
    -- trim by scanning from each end: match(s, "^%s*(.*%S)") is quadratic on an all-whitespace body
    local a = v and type(v.body) == "string" and find(v.body, "%S")
    text = a and sub(v.body, a, #v.body + 1 - find(v.body:reverse(), "%S"))
    if not text then return oops(c, B400, "body is required") end
    if #text - select(2, gsub(text, "[\128-\191]", "")) > 500 then return oops(c, B400, "body must be at most 500 characters") end
  end
  if not dirty then
    if run(q_begin) ~= DONE then error(db:errmsg()) end
    dirty = true
  end
  if id then
    q_like:bind(1, uid); q_like:bind(2, id)
    if run(q_like) ~= DONE then error(db:errmsg()) end
    if db:changes() == 1 then return reply(c, "201 Created", "\\liked\1false\2\1" .. id .. "\2") end
    q_exists:bind(1, id)
    local rc = run(q_exists)
    if rc == ROW then return reply(c, "200 OK", "\\liked\1true\2\1" .. id .. "\2") end
    if rc ~= DONE then error(db:errmsg()) end
    return oops(c, B404, "post not found")
  end
  q_ins:bind(1, uid); q_ins:bind(2, text); q_ins:bind(3, name)
  local rc = q_ins:step()
  local o = rc == ROW and q_ins:get_value(0)
  if o then rc = q_ins:step() end
  q_ins:reset()
  if rc ~= DONE then error(db:errmsg()) end
  reply(c, "201 Created", "\\post", o)
end

-- Connections. luasocket's select() cannot watch a descriptor above 1023, so the sockets are plain fds
-- on an epoll set (evio.c). Every keep-alive connection stays open until its client closes it.
local host, port = os.getenv("HOST") or "127.0.0.1", tonumber(os.getenv("PORT")) or 3000
local srv = ev.listen(host, port, 4096)
local ep = ev.epoll()
ev.add(ep, srv, 1)
local conns = {} -- fd -> connection
local sending, hungup = {}, {} -- connections with output this tick; peers that closed this tick
-- A connection reads only while it has no unsent output and no unparsed requests, and answers at most
-- PIPE pipelined requests a turn (the rest wait in c.buf, listed in more): a client that never reads
-- holds at most PIPE answers here instead of making the server buffer without limit.
local PIPE, more = 32, {}
local function arm(c)
  ev.mod(ep, c.fd, (c.rest and 2 or 0) | ((c.gone or c.rest or c.more) and 0 or 1))
end

local function close(c)
  if c.closed then return end
  c.closed = true
  conns[c.fd] = nil
  ev.close(c.fd) -- closing the fd also takes it out of the epoll set
end

-- shortcut: no Transfer-Encoding: chunked, no Expect: 100-continue, GET bodies are not skipped,
-- Connection: close is left to the client. Add them if a client needs them.
local function on_data(c, data)
  local buf = c.buf and c.buf .. data or data
  local pos, len, k = 1, #buf, 0
  while pos <= len do
    if k == PIPE then c.more = true; more[#more + 1] = c; arm(c); break end
    k = k + 1
    local _, he = find(buf, "\r\n\r\n", pos, true)
    if not he then break end
    local nxt, ok, e = he + 1
    if byte(buf, pos) == 71 then -- "G"
      ok, e = pcall(get, c, buf, pos)
    else
      local h = lower(sub(buf, pos, he))
      nxt = nxt + (tonumber(match(h, "\ncontent%-length:[ \t]*(%d+)")) or 0)
      if nxt > len + 1 then break end
      ok, e = pcall(write, c, buf, pos, h, sub(buf, he + 1, nxt - 1))
    end
    if not ok then
      io.stderr:write("request failed: ", tostring(e), "\n")
      oops(c, "500 Internal Server Error", "internal server error")
    end
    pos = nxt
  end
  if len - pos >= 1048576 then return close(c) end -- a request this large is not one of ours
  c.buf = pos <= len and sub(buf, pos) or nil
end

local function push(c, s) -- send what the socket takes now; keep the rest for when it is writable
  local n = ev.send(c.fd, s)
  if n == #s then
    if c.gone then close(c) end
    return
  end
  if not n then return close(c) end
  c.rest = sub(s, n + 1)
  arm(c)
end

local fds, evs = {}, {}
local function tick()
  local m = more
  if #m > 0 then more = {} end -- no new table on the common turn
  local n = ev.wait(ep, #m > 0 and 0 or 1000, fds, evs)
  for i = 1, #m do -- requests left in c.buf last turn; a connection still sending waits for its drain below
    local c = m[i]
    if not c.closed and not c.rest then
      c.more = nil
      on_data(c, "")
      if not c.closed and not c.more then arm(c) end
    end
  end
  for k = 1, n do
    local fd, e = fds[k], evs[k]
    if fd == srv then
      -- shortcut: if accept fails for want of descriptors (65,535 here) the listener spins until one frees.
      for _ = 1, 256 do
        local cs = ev.accept(srv)
        if not cs then break end
        conns[cs] = { fd = cs, out = {} }
        ev.add(ep, cs, 1)
      end
    else
      local c = conns[fd]
      if c and e & 2 ~= 0 and c.rest then
        local rest = c.rest
        c.rest = nil
        push(c, rest)
        if not c.closed and not c.rest then
          if c.more then more[#more + 1] = c else arm(c) end
        end
      end
      if c and not c.closed and not c.gone and not c.more and e & 1 ~= 0 then
        local d, err = ev.recv(fd)
        if d then on_data(c, d) end
        if err == "closed" then
          c.gone = true; hungup[#hungup + 1] = c
          arm(c) -- an EOF stays readable; only wait for output now
        end
      end
    end
  end
  if nj > 0 then
    tex_code[nj + 1] = "\\luafunction3 "
    tex.sprint(concat(tex_code, "", 1, nj + 1))
  else
    F[3]()
  end
end
F[1] = function()
  local ok, e = pcall(tick)
  if not ok then io.stderr:write("tick failed: ", tostring(e), "\n") end
end

-- \luafunction2: TeX has expanded the next body into \out.
local HEAD = "\r\nContent-Type: application/json\r\nContent-Length: "
F[2] = function()
  taken = taken + 1
  local body, c, inner = token.get_macro("out"), jc[taken], ji[taken]
  if inner then
    local at = find(body, "@", 1, true)
    body = sub(body, 1, at - 1) .. inner .. sub(body, at + 1)
  end
  c.out[#c.out + 1] = "HTTP/1.1 " .. js[taken] .. HEAD .. #body .. "\r\n\r\n" .. body
  if not c.queued then c.queued = true; sending[#sending + 1] = c end
end

-- \luafunction3: group commit, then one send per connection.
F[3] = function()
  local ok = true
  if dirty then
    dirty = false
    ok = run(q_commit) == DONE
    if not ok then io.stderr:write("commit failed: ", db:errmsg(), "\n"); db:exec("ROLLBACK") end
  end
  for i = 1, #sending do
    local c = sending[i]
    sending[i], c.queued = nil, nil
    local out = concat(c.out)
    c.out = {}
    if not ok then -- nothing was committed, so nobody in this tick gets an answer
      close(c)
    elseif not c.closed then
      if c.rest then c.rest = c.rest .. out else push(c, out) end
    end
  end
  for i = 1, nj do jc[i], ji[i] = nil, nil end
  nj, taken = 0, 0
  for i = 1, #hungup do -- peers that hung up: closed now, or by push() once their last bytes are out
    if not hungup[i].rest then close(hungup[i]) end
    hungup[i] = nil
  end
end

io.stdout:write("listening on ", host, ":", port, "\n")
io.stdout:flush()
