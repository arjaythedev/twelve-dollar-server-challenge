#!/usr/bin/env bash
# Runs inside the dev container as the unprivileged user. /src is the repo, mounted read-only.
# Builds from a fresh copy of the submission, so nothing from the host checkout leaks in.
set -euo pipefail
SUB=julia-raw-amanica
mkdir -p /work/submissions
for d in seed test bench; do ln -sfn "/src/$d" "/work/$d"; done
rm -rf "/work/submissions/$SUB"
cp -r "/src/submissions/$SUB" /work/submissions/
rm -rf "/work/submissions/$SUB/build" "/work/submissions/$SUB/results"
cd /work
case "${1:-test}" in
  build) bash "submissions/$SUB/build.sh" ;;
  test)  bash test/run.sh "submissions/$SUB" ;;
  serve) # build, then run the server on a fresh copy of the seed in the foreground (for stress)
         bash "submissions/$SUB/build.sh"
         mkdir -p /work/data && cp /src/seed/feed.db /work/data/feed.db
         export SQLITE_PATH=/work/data/feed.db JWT_SECRET=twelve-dollar-challenge
         export HOST="${HOST:-0.0.0.0}" PORT="${PORT:-3000}"
         exec bash "submissions/$SUB/start.sh" ;;
  *) echo "unknown: $1" >&2; exit 2 ;;
esac
