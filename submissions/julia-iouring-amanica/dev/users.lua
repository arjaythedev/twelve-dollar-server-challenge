-- GEN=users: the blast.lua request mix, but every connection pauses between requests like a k6 user,
-- so N connections ~ N users. bench/load.js averages ~2.17 requests per ~20.5 s loop (one per ~9.5 s);
-- wrk's Lua state is per thread, not per connection, so think time is drawn independently per request:
-- uniform 3-16 s (mean 9.5 s) instead of load.js's exact sequence.
dofile("/blast.lua")

function delay()
  return math.random(3000, 16000)
end
