#!/usr/bin/env bash
# Runs inside the dev container as the unprivileged user. /src is the repo, mounted read-only.
# Builds from a fresh copy of the submission, so nothing from the host checkout leaks in.
set -euo pipefail
SUB="${JR_ENTRY:-$(basename "$(cd "$(dirname "$0")/.." && pwd)")}"   # another entry, for calibration (dev/ct.sh ENTRY=)
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
         mkdir -p /work/data && rm -f /work/data/feed.db*
         if [ -n "${JR_BTDB:-}" ]; then cp "/src/submissions/$SUB/$JR_BTDB"/feed.db* /work/data/
         else cp /src/seed/feed.db /work/data/feed.db; fi
         . "submissions/$SUB/config.sh"
         exec "$JULIA_HOME/bin/julia" --startup-file=no -O3 "submissions/$SUB/dev/feedbench.jl" /work/data/feed.db ;;
  btreediff) # dev/btreediff.jl: B-tree reads vs the SQL statements, byte for byte, on a fresh seed copy
         bash "submissions/$SUB/build.sh" >/dev/null
         mkdir -p /work/data && cp /src/seed/feed.db /work/data/feed.db
         . "submissions/$SUB/config.sh"
         exec "$JULIA_HOME/bin/julia" --startup-file=no -O3 "submissions/$SUB/dev/btreediff.jl" /work/data/feed.db ;;
  jwtbench) # dev/jwtbench.jl: HS256 signature variants (PLAN 7.1)
         bash "submissions/$SUB/build.sh" >/dev/null
         . "submissions/$SUB/config.sh"
         exec "$JULIA_HOME/bin/julia" --startup-file=no -O3 "submissions/$SUB/dev/jwtbench.jl" ;;
  feedbench-pgo) # SQLite PGO feasibility (PLAN 5.8): plain, then instrumented + trained on feedbench, then
         # profile-guided, each timed with feedbench. Trained and measured on the same workload: an upper bound.
         . "submissions/$SUB/config.sh"
         fb() { mkdir -p /work/data && cp /src/seed/feed.db /work/data/feed.db && rm -f /work/data/feed.db-wal /work/data/feed.db-shm
                "$JULIA_HOME/bin/julia" --startup-file=no -O3 "submissions/$SUB/dev/feedbench.jl" /work/data/feed.db; }
         P=/work/pgo; rm -rf "$P"
         echo "#### plain -O2"; JR_SQLITE_OPT="-O2" bash "submissions/$SUB/build.sh" >/dev/null; fb
         echo "#### training run (instrumented)"
         JR_SQLITE_OPT="-O2 -fprofile-generate -fprofile-update=single -fprofile-dir=$P" bash "submissions/$SUB/build.sh" >/dev/null
         fb >/dev/null; echo "profile files: $(find "$P" -name '*.gcda' | wc -l)"
         echo "#### -O2 + PGO"
         JR_SQLITE_OPT="-O2 -fprofile-use -fprofile-partial-training -Wno-missing-profile -fprofile-dir=$P" bash "submissions/$SUB/build.sh" >/dev/null; fb ;;
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
