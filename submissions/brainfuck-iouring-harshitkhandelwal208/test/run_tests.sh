#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
SUB_ROOT="$(dirname "$DIR")"

echo "================================================================================"
echo "RUNNING FULL VERIFICATION SUITE"
echo "================================================================================"

echo "==> 1. Verifying Self-Hosting Compiler Build Pipeline..."
"$SUB_ROOT/build.sh"

echo "==> 2. Running Differential Compiler Testing (Interpreter vs Stage0 vs Stage1 vs Stage2)..."
python3 "$DIR/test_differential.py"

echo "==> 3. Running HTTP Runtime Correctness Tests (Fragmentation, Pipelining, Isolation)..."
python3 "$DIR/test_runtime_correctness.py"

echo "==> 4. Checking for Challenge Database to Run Server Integration Tests..."
SEED_DB=""
if [ -f "$SUB_ROOT/../../seed/feed.db" ]; then
    SEED_DB="$SUB_ROOT/../../seed/feed.db"
elif [ -f "/tmp/feed_seed.db" ]; then
    SEED_DB="/tmp/feed_seed.db"
elif [ -f "/tmp/twelve-challenge/seed/feed.db" ]; then
    SEED_DB="/tmp/twelve-challenge/seed/feed.db"
fi

TEST_SCRIPT=""
if [ -f "$SUB_ROOT/../../test/test.sh" ]; then
    TEST_SCRIPT="$SUB_ROOT/../../test/test.sh"
elif [ -f "/tmp/twelve-challenge/test/test.sh" ]; then
    TEST_SCRIPT="/tmp/twelve-challenge/test/test.sh"
fi

if [ -n "$SEED_DB" ] && [ -n "$TEST_SCRIPT" ]; then
    echo "==> Running Full 42-Test Challenge Suite against Production Server..."
    SCRATCH_DB="/tmp/test_feed_run_$$.db"
    rm -f "$SCRATCH_DB"*
    cp "$SEED_DB" "$SCRATCH_DB"
    PORT=13000 SQLITE_PATH="$SCRATCH_DB" "$SUB_ROOT/bin/server" >/dev/null 2>&1 &
    SERVER_PID=$!
    sleep 1
    STATUS=0
    bash "$TEST_SCRIPT" "http://127.0.0.1:13000" || STATUS=$?
    kill -9 "$SERVER_PID" 2>/dev/null || true
    rm -f "$SCRATCH_DB"*
    if [ $STATUS -ne 0 ]; then
        echo "Server integration tests failed!"
        exit $STATUS
    fi
    echo "All challenge tests passed!"
else
    echo "Notice: Challenge test suite / seed DB not present in standard location. Skipping server integration step."
fi

echo "================================================================================"
echo "ALL TESTS PASSED SUCCESSFULLY!"
echo "================================================================================"
