-- Saturation diagnostic with the challenge's 100:100:15:2 request proportions.
-- Removes think time; capacity still requires the unchanged k6 workload.
local serial = 0
local workers = {}
function setup(thread)
  serial = serial + 1
  thread:set("worker", serial)
  workers[#workers + 1] = thread
end

local tokens, ids, bad = {}, {1}, 0
function init(args)
  math.randomseed(1409 + worker)
  local file = assert(io.open(args[1], "r"))
  local data = file:read("*a")
  file:close()
  for token in data:gmatch('"token"%s*:%s*"([^"]+)"') do
    tokens[#tokens + 1] = token
  end
  assert(#tokens > 0)
end

function request()
  local choice = math.random(217)
  if choice <= 100 then
    return wrk.format("GET", "/feed")
  end
  local id = ids[math.random(#ids)]
  if choice <= 200 then
    return wrk.format("GET", "/posts/" .. id)
  end
  local headers = {Authorization = "Bearer " .. tokens[math.random(#tokens)],
                   ["Content-Type"] = "application/json"}
  if choice <= 215 then
    return wrk.format("POST", "/posts/" .. id .. "/like", headers, "")
  end
  return wrk.format("POST", "/posts", headers,
                   '{"body":"Synthetic mixed profiling request"}')
end

function response(status, headers, body)
  local valid = status == 200 or status == 201
  -- wrk shares Lua state across connections: a thread-global last request
  -- cannot identify the request corresponding to an out-of-order response.
  if body:match('^%{"posts":%[') and status == 200 then
    local found = {}
    for id in body:gmatch('"id":(%d+)') do found[#found + 1] = id end
    valid = #found == 20
    if valid then ids = found end
  end
  if not valid then bad = bad + 1 end
  invalid_responses = bad
end

function done(summary, latency, requests)
  local invalid = 0
  for _, thread in ipairs(workers) do
    invalid = invalid + (thread:get("invalid_responses") or 0)
  end
  io.write(string.format("p95_us=%d p99_us=%d requests=%d invalid=%d\n",
      latency:percentile(95), latency:percentile(99), summary.requests, invalid))
end
