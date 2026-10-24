#!/usr/bin/env bash
# Runs everything: unit tests, the fuzz pass, the challenge suite and the
# black-box suite against one build of the server.
#
#   test/run_all.sh [asan|plain|release] [--long]
#
# Needs ../../seed/feed.db (bash seed/make-seed.sh in the repo root).
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-asan}"
LONG="${2:-}"
SEED=../../seed/feed.db
[ -f "$SEED" ] || { echo "missing $SEED"; exit 1; }
case "$MODE" in
  asan) make -s asan; BIN=bin/server-asan ;;
  plain) make -s plain; BIN=bin/server ;;
  release) make -s release; BIN=bin/server ;;
  *) echo "mode: asan|plain|release"; exit 2 ;;
esac
echo "== unit tests"; make -s check
echo "== fuzz"; make -s fuzz FUZZ_ITERS=300000
echo "== challenge suite ($BIN)"
DB="$(mktemp -d)/feed.db"
cp "$SEED" "$DB"
PORT=$((20000 + RANDOM % 20000))
SQLITE_PATH="$DB" JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$PORT "$BIN" >"$(dirname "$DB")/server.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; wait $PID 2>/dev/null || true; rm -rf "$(dirname "$DB")"' EXIT
for _ in $(seq 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
bash ../../test/test.sh "http://127.0.0.1:$PORT"
kill -TERM $PID; wait $PID
if grep -q 'ERROR: AddressSanitizer\|runtime error' "$(dirname "$DB")/server.log"; then
  echo "sanitizer findings:"; grep -A5 'ERROR\|runtime error' "$(dirname "$DB")/server.log"; exit 1
fi
trap - EXIT; rm -rf "$(dirname "$DB")"
echo "== black-box suite"
python3 test/http_tests.py --server "$BIN" --seed "$SEED" ${LONG:+--long}
