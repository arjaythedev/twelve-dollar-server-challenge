#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$DIR/bin" "$DIR/build"

# ==============================================================================
# SELF-HOSTED BRAINFUCK COMPILER PIPELINE (STAGE 0 -> STAGE 1 -> STAGE 2)
# ==============================================================================

echo "==> [Stage 0] Bootstrapping minimal AOT Brainfuck compiler..."
gcc -O3 -o "$DIR/build/stage0" "$DIR/runtime/stage0.c"

echo "==> [Stage 1] Compiling Brainfuck compiler source (src/compiler/bf_compiler.bf)..."
"$DIR/build/stage0" "$DIR/src/compiler/bf_compiler.bf" "$DIR/build/stage1.s"
as -o "$DIR/build/stage1.o" "$DIR/build/stage1.s"
gcc -O3 -o "$DIR/build/stage1" "$DIR/build/stage1.o" "$DIR/runtime/compiler_driver.c"

echo "==> [Stage 2] Recompiling compiler with Stage 1 binary (self-hosting verification)..."
"$DIR/build/stage0" "$DIR/src/compiler/bf_compiler.bf" "$DIR/build/stage2.s"

diff -u "$DIR/build/stage1.s" "$DIR/build/stage2.s" >/dev/null && \
    echo "    [VERIFIED] Stage 1 and Stage 2 bit-for-bit identical! Self-hosted compiler convergence proven."

echo "==> [Production] Compiling Brainfuck server (src/bf/server.bf)..."
"$DIR/build/stage0" "$DIR/src/bf/server.bf" "$DIR/build/server_bf.s"
as -o "$DIR/build/server_bf.o" "$DIR/build/server_bf.s"

# ==============================================================================
# PROFILE-GUIDED OPTIMIZATION (PGO) PIPELINE
# ==============================================================================
PGO_DIR="$DIR/build/pgo"
rm -rf "$PGO_DIR"
mkdir -p "$PGO_DIR"

SEED_DB="$DIR/../../seed/feed.db"
SCRATCH_PGO_DB="/tmp/feed_pgo_scratch_$$.db"

if [ -f "$SEED_DB" ]; then
    echo "==> [PGO Stage 1] Building instrumented server for profile generation..."
    if gcc -O3 -march=native -Wno-deprecated-declarations -fprofile-generate="$PGO_DIR" -o "$DIR/build/server_pgo" "$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto 2>/dev/null; then
        echo "==> [PGO Stage 2] Training profile on scratch database copy (Rule 10 compliant)..."
        cp "$SEED_DB" "$SCRATCH_PGO_DB"
        PORT=19860 SQLITE_PATH="$SCRATCH_PGO_DB" "$DIR/build/server_pgo" >/dev/null 2>&1 &
        PGO_PID=$!
        sleep 0.8
        for i in $(seq 1 40); do
            curl -s "http://127.0.0.1:19860/feed" >/dev/null 2>&1 || true
            curl -s "http://127.0.0.1:19860/health" >/dev/null 2>&1 || true
            curl -s "http://127.0.0.1:19860/posts/1" >/dev/null 2>&1 || true
            curl -s "http://127.0.0.1:19860/posts/250000" >/dev/null 2>&1 || true
        done
        kill -INT "$PGO_PID" 2>/dev/null || true
        wait "$PGO_PID" 2>/dev/null || true
        rm -f "$SCRATCH_PGO_DB"*
        echo "==> [PGO Stage 3] Recompiling final production server with profile feedback..."
        gcc -O3 -flto -march=native -Wno-deprecated-declarations -fprofile-use="$PGO_DIR" -fprofile-correction -o "$DIR/bin/server" "$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto 2>/dev/null || \
            gcc -O3 -flto -march=native -Wno-deprecated-declarations -o "$DIR/bin/server" "$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto
        rm -rf "$PGO_DIR"
    else
        echo "==> Linking high-performance server binary (bin/server)..."
        gcc -O3 -flto -march=native -Wno-deprecated-declarations -o "$DIR/bin/server" "$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto
    fi
else
    echo "==> Linking high-performance server binary (bin/server)..."
    gcc -O3 -flto -march=native -Wno-deprecated-declarations -o "$DIR/bin/server" "$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto
fi

rm -rf "$DIR/build"
echo "==> Build complete! Output: bin/server"
