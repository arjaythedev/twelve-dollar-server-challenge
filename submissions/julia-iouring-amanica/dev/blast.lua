-- GEN=blast: the bench/load.js request mix with no think time (closed loop, max throughput).
-- Per simulated user loop: GET /feed, GET /posts/:id (an id from a recent feed), 15% like, 2% create,
-- so the weights are 1 : 1 : 0.15 : 0.02. Each wrk connection is one "user" with no pauses.
local tokens, names, ids, nids = {}, {}, {}, 0

function init(args)
  local f = assert(io.open("/seed/tokens.json"))
  local s = f:read("*a"); f:close()
  for name, tok in s:gmatch('"username":"([^"]+)","token":"([^"]+)"') do
    tokens[#tokens + 1] = tok; names[#names + 1] = name
  end
  assert(#tokens > 0, "no tokens parsed from /seed/tokens.json")
  math.randomseed(os.time() + tonumber(tostring({}):match("0x(%x+)") or "0", 16) % 100000)
end

local function some_id()
  if nids == 0 then return "500000" end
  return ids[math.random(nids)]
end

function request()
  local x = math.random() * 2.17
  if x < 1 or nids == 0 then
    return wrk.format("GET", "/feed")
  elseif x < 2 then
    return wrk.format("GET", "/posts/" .. some_id())
  end
  local u = math.random(#tokens)
  local h = { ["Authorization"] = "Bearer " .. tokens[u] }
  if x < 2.15 then
    return wrk.format("POST", "/posts/" .. some_id() .. "/like", h)
  end
  h["Content-Type"] = "application/json"
  return wrk.format("POST", "/posts", h,
    string.format('{"body":"%s says hi at %d (blast %d)"}', names[u], os.time(), math.random(1e9)))
end

function response(status, headers, body)
  if status == 200 and body:sub(1, 10) == '{"posts":[' then
    local n = 0
    for id in body:gmatch('{"id":(%d+)') do n = n + 1; ids[n] = id end
    if n > 0 then nids = n end
  end
end

function done(s, lat, req)
  io.write(string.format(
    'BLAST_JSON {"requests":%d,"duration_us":%d,"err_status":%d,"err_connect":%d,"err_read":%d,' ..
    '"err_write":%d,"err_timeout":%d,"p50_us":%d,"p95_us":%d,"p99_us":%d,"max_us":%d,"mean_us":%.1f}\n',
    s.requests, s.duration, s.errors.status, s.errors.connect, s.errors.read, s.errors.write,
    s.errors.timeout, lat:percentile(50), lat:percentile(95), lat:percentile(99), lat.max, lat.mean))
  -- 200 latency quantiles (0.5% steps), so several wrk shards can be merged (GEN=users)
  local q = {}
  for i = 1, 200 do q[i] = string.format("%d", lat:percentile(i * 0.5)) end
  io.write('BLAST_Q [' .. table.concat(q, ",") .. ']\n')
end
