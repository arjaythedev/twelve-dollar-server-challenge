-- Mixed workload with bench/load.js request proportions: 100 feed : 100 post : 15 like : 2 create.
-- Post ids come from the newest posts (like the real loop, which reads ids off the feed).
local tokens = {}
local f = io.open(os.getenv("TOKENS") or "../../../seed/tokens.json"); local s = f:read("*a"); f:close()
for t in s:gmatch('"token"%s*:%s*"([^"]+)"') do tokens[#tokens+1] = t end
local counter = 0
function setup(thread) counter = counter + 1; thread:set("tid", counter) end
local reqs = {}
function init(args)
  math.randomseed(tid * 7919 + os.time())
  local tok = tokens[(tid * 97) % #tokens + 1]
  local auth = {["Authorization"] = "Bearer " .. tok, ["Content-Type"] = "application/json"}
  local mode = args[1] or "mix"
  local close = args[2] == "close"
  local plain = close and {["Connection"] = "close"} or nil
  if close then auth["Connection"] = "close" end
  for i = 1, 4000 do
    local r = math.random() * 217
    local id = 499800 + math.random(0, 200)
    if mode == "feed" then r = 0 elseif mode == "post" then r = 150 elseif mode == "like" then r = 210; id = math.random(1, 500000) elseif mode == "create" then r = 216 end
    if r < 100 then reqs[i] = wrk.format("GET", "/feed", plain)
    elseif r < 200 then reqs[i] = wrk.format("GET", "/posts/" .. id, plain)
    elseif r < 215 then reqs[i] = wrk.format("POST", "/posts/" .. id .. "/like", auth, "")
    else reqs[i] = wrk.format("POST", "/posts", auth, '{"body":"wrk thread ' .. tid .. ' says hi ' .. i .. '"}') end
  end
end
local n = 0
function request() n = n % #reqs + 1; return reqs[n] end
local bad = 0
function response(status) if status >= 400 then bad = bad + 1 end end
function done(summary, latency, requests)
  io.write(string.format("RESULT rps=%.0f p50=%.2fms p99=%.2fms non2xx_errors=%d\n",
    summary.requests / (summary.duration / 1e6), latency:percentile(50) / 1000, latency:percentile(99) / 1000,
    summary.errors.status + summary.errors.connect + summary.errors.read + summary.errors.write + summary.errors.timeout))
end
