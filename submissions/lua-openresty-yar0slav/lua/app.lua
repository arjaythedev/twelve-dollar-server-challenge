-- The feed API, running inside nginx's content phase. One module: SQLite through LuaJIT's FFI,
-- HS256 JWT verification through OpenSSL's HMAC (also FFI), and JSON built by concatenation so
-- the key order is exactly the spec's.
local ffi = require "ffi"
local b64 = require "ngx.base64"
local json = require("cjson.safe").new()
json.encode_escape_forward_slash(false)

local C = ffi.C
local ngx = ngx
local type, tonumber, tostring, pcall = type, tonumber, tostring, pcall
local concat, floor = table.concat, math.floor

ffi.cdef [[
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char *filename, sqlite3 **db, int flags, const char *vfs);
int sqlite3_exec(sqlite3 *db, const char *sql, void *cb, void *arg, char **errmsg);
int sqlite3_busy_timeout(sqlite3 *db, int ms);
int sqlite3_prepare_v3(sqlite3 *db, const char *sql, int n, unsigned int flags, sqlite3_stmt **stmt, const char **tail);
int sqlite3_bind_int64(sqlite3_stmt *stmt, int i, int64_t v);
int sqlite3_bind_text(sqlite3_stmt *stmt, int i, const char *v, int n, void (*destructor)(void *));
int sqlite3_step(sqlite3_stmt *stmt);
int sqlite3_reset(sqlite3_stmt *stmt);
int64_t sqlite3_column_int64(sqlite3_stmt *stmt, int col);
const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int col);
int sqlite3_column_bytes(sqlite3_stmt *stmt, int col);
int sqlite3_changes(sqlite3 *db);
const char *sqlite3_errmsg(sqlite3 *db);

typedef struct evp_md_st EVP_MD;
const EVP_MD *EVP_sha256(void);
unsigned char *HMAC(const EVP_MD *md, const void *key, int key_len, const unsigned char *d, size_t n,
                    unsigned char *out, unsigned int *out_len);
]]

local sqlite = ffi.load("libsqlite3.so.0")
-- OpenResty's nginx links its own OpenSSL, so HMAC is already in the process.
local crypto = pcall(function() return C.HMAC end) and C
  or ffi.load("/usr/local/openresty/openssl3/lib/libcrypto.so.3")

local ROW, DONE = 100, 101
local OPEN_READWRITE, OPEN_NOMUTEX, PREPARE_PERSISTENT = 0x2, 0x8000, 0x1
local TRANSIENT = ffi.cast("void (*)(void *)", -1)

local POST_SQL = [[
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id ]]

local SQL = {
  post   = POST_SQL .. "WHERE p.id = ?1",
  feed   = POST_SQL .. "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
  create = "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
  -- inserts nothing if the post doesn't exist, or if this user already liked it
  like   = [[INSERT INTO likes (user_id, post_id) SELECT ?1, ?2
              WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
              ON CONFLICT (user_id, post_id) DO NOTHING]],
  exists = "SELECT 1 FROM posts WHERE id = ?1",
  health = "SELECT 1",
}

local started = ngx.now()
local db, st, secret, sha256
local md, md_len = ffi.new("unsigned char[32]"), ffi.new("unsigned int[1]")

local _M = {}

local function db_error()
  return db and ffi.string(sqlite.sqlite3_errmsg(db)) or "database is not open"
end

-- Runs in the worker process: SQLite handles must never cross nginx's fork.
function _M.init_worker()
  secret = os.getenv("JWT_SECRET") or ""
  sha256 = crypto.EVP_sha256()

  local pdb = ffi.new("sqlite3*[1]")
  if sqlite.sqlite3_open_v2(os.getenv("SQLITE_PATH") or "", pdb, OPEN_READWRITE + OPEN_NOMUTEX, nil) ~= 0 then
    ngx.log(ngx.ERR, "cannot open SQLITE_PATH: ", pdb[0] ~= nil and ffi.string(sqlite.sqlite3_errmsg(pdb[0])) or "")
    return
  end
  db = pdb[0]
  sqlite.sqlite3_busy_timeout(db, 5000)
  local rc = sqlite.sqlite3_exec(db, [[
    PRAGMA journal_mode = WAL;
    PRAGMA synchronous = NORMAL;
    PRAGMA cache_size = -65536;
    PRAGMA mmap_size = 1073741824;
    PRAGMA temp_store = MEMORY;
  ]], nil, nil, nil)
  if rc ~= 0 then ngx.log(ngx.ERR, "pragmas: ", db_error()) end

  st = {}
  local pstmt = ffi.new("sqlite3_stmt*[1]")
  for name, sql in pairs(SQL) do
    if sqlite.sqlite3_prepare_v3(db, sql, #sql, PREPARE_PERSISTENT, pstmt, nil) ~= 0 then
      ngx.log(ngx.ERR, "prepare ", name, ": ", db_error())
      db = nil
      return
    end
    st[name] = pstmt[0]
  end
end

-- responses ----------------------------------------------------------------------------------

local function send(status, body)
  ngx.status = status
  ngx.header.content_length = #body
  ngx.print(body)
end

local function fail(status, message)
  send(status, '{"error":"' .. message .. '"}')
end

-- sqlite helpers -----------------------------------------------------------------------------

local function text(s, col)
  local p = sqlite.sqlite3_column_text(s, col)
  if p == nil then return "" end
  return ffi.string(p, sqlite.sqlite3_column_bytes(s, col))
end

local function int(s, col)
  return tonumber(sqlite.sqlite3_column_int64(s, col))
end

-- steps a statement; anything but a row or done is an error (reset first so it can be reused)
local function step(s)
  local rc = sqlite.sqlite3_step(s)
  if rc ~= ROW and rc ~= DONE then
    sqlite.sqlite3_reset(s)
    error("sqlite step: " .. db_error())
  end
  return rc
end

local function post_json(s)
  return '{"id":' .. int(s, 0)
      .. ',"body":' .. json.encode(text(s, 1))
      .. ',"created_at":"' .. text(s, 2)
      .. '","author":' .. json.encode(text(s, 3))
      .. ',"like_count":' .. int(s, 4) .. '}'
end

-- a positive integer id, or nil ("0", "-1", "abc", "1.5")
local function parse_id(s)
  if s and #s <= 16 and s:find("^[1-9]%d*$") then return tonumber(s) end
end

-- auth ---------------------------------------------------------------------------------------

local function b64_json(s)
  local raw = b64.decode_base64url(s)
  return raw and json.decode(raw)
end

-- returns user id, username; or nil, error message. Verifies the signature on every request.
local function authenticate()
  local h = ngx.var.http_authorization
  if not h or h:sub(1, 7) ~= "Bearer " then return nil, "missing bearer token" end
  local token = h:sub(8)

  local d1 = token:find(".", 1, true)
  local d2 = d1 and token:find(".", d1 + 1, true)
  if not d2 or token:find(".", d2 + 1, true) then return nil, "invalid or expired token" end

  local signed = token:sub(1, d2 - 1)
  crypto.HMAC(sha256, secret, #secret, signed, #signed, md, md_len)
  if b64.encode_base64url(ffi.string(md, md_len[0])) ~= token:sub(d2 + 1) then
    return nil, "invalid or expired token"
  end

  local header = b64_json(token:sub(1, d1 - 1))
  if type(header) ~= "table" or header.alg ~= "HS256" then return nil, "invalid or expired token" end
  local claims = b64_json(token:sub(d1 + 1, d2 - 1))
  if type(claims) ~= "table" then return nil, "invalid or expired token" end
  local now = ngx.time()
  if type(claims.exp) ~= "number" or claims.exp <= now then return nil, "invalid or expired token" end
  if type(claims.nbf) == "number" and claims.nbf > now then return nil, "invalid or expired token" end

  local uid = type(claims.sub) == "string" and parse_id(claims.sub)
  if not uid or type(claims.username) ~= "string" then return nil, "invalid token payload" end
  return uid, claims.username
end

-- handlers -----------------------------------------------------------------------------------

local function health()
  local ok, err = pcall(function()
    if not db then error(db_error()) end
    step(st.health)
    sqlite.sqlite3_reset(st.health)
  end)
  if ok then
    send(200, '{"status":"ok","db":"ok","uptime_s":' .. floor(ngx.now() - started) .. '}')
  else
    send(503, '{"status":"degraded","db":"unreachable","error":' .. json.encode(tostring(err)) .. '}')
  end
end

local function feed()
  local s, posts, n = st.feed, {}, 0
  while step(s) == ROW do
    n = n + 1
    posts[n] = post_json(s)
  end
  sqlite.sqlite3_reset(s)
  send(200, '{"posts":[' .. concat(posts, ",") .. ']}')
end

local function get_post(raw_id)
  local id = parse_id(raw_id)
  if not id then return fail(400, "invalid post id") end
  local s = st.post
  sqlite.sqlite3_bind_int64(s, 1, id)
  if step(s) ~= ROW then
    sqlite.sqlite3_reset(s)
    return fail(404, "post not found")
  end
  local body = '{"post":' .. post_json(s) .. '}'
  sqlite.sqlite3_reset(s)
  send(200, body)
end

local function create_post()
  local uid, username = authenticate()
  if not uid then return fail(401, username) end

  ngx.req.read_body()
  local raw = ngx.req.get_body_data()
  local req = raw and json.decode(raw)
  if req == nil then return fail(400, "malformed JSON body") end
  local body = type(req) == "table" and req.body
  if type(body) ~= "string" then return fail(400, "body is required") end

  local first = body:find("%S")
  if not first then return fail(400, "body is required") end
  local last = #body
  while body:find("^%s", last) do last = last - 1 end
  body = body:sub(first, last)
  -- 500 characters, not bytes: count every byte that isn't a UTF-8 continuation byte
  if #body > 500 and select(2, body:gsub("[^\128-\191]", "")) > 500 then
    return fail(400, "body must be at most 500 characters")
  end

  local s = st.create
  sqlite.sqlite3_bind_int64(s, 1, uid)
  sqlite.sqlite3_bind_text(s, 2, body, #body, TRANSIENT)
  if step(s) ~= ROW then
    sqlite.sqlite3_reset(s)
    error("insert returned no row")
  end
  local id, created_at = int(s, 0), text(s, 1)
  step(s)  -- runs the statement to completion: the autocommit transaction commits here
  sqlite.sqlite3_reset(s)

  send(201, '{"post":{"id":' .. id .. ',"body":' .. json.encode(body)
    .. ',"created_at":"' .. created_at .. '","author":' .. json.encode(username)
    .. ',"like_count":0}}')
end

local function like(raw_id)
  local uid, err = authenticate()
  if not uid then return fail(401, err) end
  local id = parse_id(raw_id)
  if not id then return fail(400, "invalid post id") end

  local s = st.like
  sqlite.sqlite3_bind_int64(s, 1, uid)
  sqlite.sqlite3_bind_int64(s, 2, id)
  step(s)
  local inserted = sqlite.sqlite3_changes(db) > 0
  sqlite.sqlite3_reset(s)

  if not inserted then
    -- nothing inserted: either an existing like, or no such post
    local e = st.exists
    sqlite.sqlite3_bind_int64(e, 1, id)
    local found = step(e) == ROW
    sqlite.sqlite3_reset(e)
    if not found then return fail(404, "post not found") end
  end
  send(inserted and 201 or 200, '{"liked":true,"already_liked":' .. tostring(not inserted)
    .. ',"post_id":' .. id .. '}')
end

local function route()
  local uri, method = ngx.var.uri, ngx.req.get_method()
  if uri == "/feed" then
    if method == "GET" then return feed() end
  elseif uri == "/health" then
    if method == "GET" then return health() end
  elseif uri == "/posts" then
    if method == "POST" then return create_post() end
  elseif method == "GET" then
    local id = uri:match("^/posts/([^/]+)$")
    if id then return get_post(id) end
  elseif method == "POST" then
    local id = uri:match("^/posts/([^/]+)/like$")
    if id then return like(id) end
  end
  fail(404, "not found")
end

function _M.handle()
  if not db then
    if ngx.var.uri == "/health" then return health() end
    return fail(500, "internal server error")
  end
  local ok, err = pcall(route)
  if not ok then
    ngx.log(ngx.ERR, err)
    fail(500, "internal server error")
  end
end

return _M
