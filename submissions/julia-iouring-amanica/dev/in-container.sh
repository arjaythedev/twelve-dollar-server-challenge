#!/usr/bin/env bash
# Runs inside the dev container as the unprivileged user. /src is the repo, mounted read-only.
# Builds from a fresh copy of the submission, so nothing from the host checkout leaks in.
set -euo pipefail
SUB=julia-iouring-amanica
mkdir -p /work/submissions
for d in seed test bench; do ln -sfn "/src/$d" "/work/$d"; done
rm -rf "/work/submissions/$SUB"
cp -r "/src/submissions/$SUB" /work/submissions/
rm -rf "/work/submissions/$SUB/build" "/work/submissions/$SUB/results"
cd /work
case "${1:-test}" in
  build) bash "submissions/$SUB/build.sh" ;;
  test)  bash test/run.sh "submissions/$SUB" ;;
  feedbench) # dev/feedbench.jl on a fresh seed copy (PLAN 5.5)
         bash "submissions/$SUB/build.sh" >/dev/null
         mkdir -p /work/data && cp /src/seed/feed.db /work/data/feed.db
         . "submissions/$SUB/config.sh"
         exec "$JULIA_HOME/bin/julia" --startup-file=no -O3 "submissions/$SUB/dev/feedbench.jl" /work/data/feed.db ;;
  serve) # build, then run the server on a fresh copy of the seed in the foreground (for stress)
         bash "submissions/$SUB/build.sh"
         # JR_DATA: where the database copy lives. dev/ct.sh mounts a volume (host ext4) at /data; the
         # container's own filesystem is fuse-overlayfs under rootless podman, which made every SQLite
         # write a round trip to a userspace daemon and blocked the event loop ~20% of the time.
         data="${JR_DATA:-/work/data}"
         mkdir -p "$data" && cp /src/seed/feed.db "$data/feed.db"
         export SQLITE_PATH="$data/feed.db" JWT_SECRET=twelve-dollar-challenge
         export HOST="${HOST:-0.0.0.0}" PORT="${PORT:-3000}"
         # boot time: start.sh exec -> first healthy /health, printed into the server log
         t0="$(date +%s%N)"
         ( until curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; do sleep 0.05; done
           echo "boot_ms=$(( ($(date +%s%N) - t0) / 1000000 ))" ) &
         exec bash "submissions/$SUB/start.sh" ;;
  *) echo "unknown: $1" >&2; exit 2 ;;
esac
