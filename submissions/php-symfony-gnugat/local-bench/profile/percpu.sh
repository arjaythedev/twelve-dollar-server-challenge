#!/usr/bin/env bash
# CPU per request by endpoint, without a profiler: FrankenPHP's own counters in /proc (all its threads, Go included,
# 10 ms resolution). Requests are sent one at a time from inside the container: 300 to warm up, then 1,000 measured.
#
#   bash local-bench/profile/percpu.sh <image> [Caddyfile]   # e.g. a phase's local-bench/Caddyfile.worker
#
# create: POST /posts. mix: GET /feed, then GET /posts/{id}, then a like one time in 7, like a k6 user.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="$1" SERVE_ARGS="${2:+-v $2:/app/Caddyfile:ro}" bash "$HERE/../bench.sh" start >/dev/null
docker exec twelve-dollar-server bash -c '
cpu() { awk "{print \$14+\$15}" /proc/$(pgrep -o frankenphp)/stat; }
ids=($(curl -s localhost/feed | jq -r ".posts[].id"))
mapfile -t tokens < <(jq -r ".[].token" /seed/tokens.json | head -1000)
feed() { curl -s -o /dev/null localhost/feed; }
show() { curl -s -o /dev/null localhost/posts/${ids[$((i % ${#ids[@]}))]}; }
like() { curl -s -o /dev/null -X POST -H "Authorization: Bearer ${tokens[$((i % 1000))]}" localhost/posts/${ids[$((i % ${#ids[@]}))]}/like; }
create() { curl -s -o /dev/null -X POST -H "Authorization: Bearer ${tokens[$((i % 1000))]}" -H "Content-Type: application/json" -d "{\"body\": \"percpu says hi ($i)\"}" localhost/posts; }
mix() { feed; show; [ $((i % 7)) = 0 ] && like || true; }
for endpoint in feed show like create mix; do
  for i in $(seq 300); do $endpoint; done
  before=$(cpu); for i in $(seq 301 1300); do $endpoint; done; after=$(cpu)
  echo "$endpoint: $(( (after - before) * 10 )) ms of CPU per 1,000"
done'
bash "$HERE/../bench.sh" stop
