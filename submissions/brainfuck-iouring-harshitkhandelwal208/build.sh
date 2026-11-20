#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$DIR/build"
mkdir -p "$DIR/bin" "$DIR/build"

# ==============================================================================
# SELF-HOSTED BRAINFUCK COMPILER PIPELINE (STAGE 0 -> STAGE 1 -> STAGE 2)
# ==============================================================================

echo "==> [Stage 0] Bootstrapping minimal AOT Brainfuck compiler..."
gcc -O3 -o "$DIR/build/stage0" "$DIR/runtime/stage0.c"

echo "==> [Stage 1] Compiling Brainfuck compiler source (src/compiler/bf_compiler.bf)..."
"$DIR/build/stage0" "$DIR/src/compiler/bf_compiler.bf" "$DIR/build/stage1.s"
as -o "$DIR/build/stage1.o" "$DIR/build/stage1.s"
gcc -O3 -o "$DIR/build/stage1" "$DIR/build/stage1.o"

echo "==> [Stage 2] Recompiling compiler with Stage 1 binary (self-hosting verification)..."
"$DIR/build/stage1" < "$DIR/src/compiler/bf_compiler.bf" > "$DIR/build/stage2.s"
as -o "$DIR/build/stage2.o" "$DIR/build/stage2.s"
gcc -O3 -o "$DIR/build/stage2" "$DIR/build/stage2.o"

echo "==> [Verification] Verifying bit-for-bit compiler convergence..."
if diff -u "$DIR/build/stage1.s" "$DIR/build/stage2.s" >/dev/null; then
    echo "    [VERIFIED] Stage 1 and Stage 2 bit-for-bit identical! Self-hosted compiler convergence proven."
else
    echo "    [ERROR] Self-hosting verification failed: Stage 1 and Stage 2 differ!"
    exit 1
fi

echo "==> [Production] Compiling Brainfuck server using Pure Brainfuck Compiler (stage1)..."
"$DIR/build/stage1" < "$DIR/src/bf/server.bf" > "$DIR/build/server_bf.s"
python3 "$DIR/runtime/opt_bf_asm.py" "$DIR/build/server_bf.s" "$DIR/build/server_bf.s" "$DIR/src/bf/server.bf"
as -o "$DIR/build/server_bf.o" "$DIR/build/server_bf.s"

# ==============================================================================
# PROFILE-GUIDED OPTIMIZATION (PGO) PIPELINE
# ==============================================================================
PGO_DIR="$DIR/build/pgo"
SERVER="$DIR/build/server"
SEED_DB="$DIR/../../seed/feed.db"
if [[ ! -f "$SEED_DB" && -f "$DIR/feed.db" ]]; then
    SEED_DB="$DIR/feed.db"
fi

common=(-O3 -march=native -Wno-deprecated-declarations)
sources=("$DIR/runtime/runtime.c" "$DIR/build/server_bf.o" -lsqlite3 -lcrypto)
final=(-flto -fno-plt -fno-semantic-interposition)
profile=()

train_profile() (
    set -euo pipefail
    scratch=$(mktemp -d "$DIR/build/pgo-training.XXXXXXXX")
    pid=
    cleanup_training() {
        if [[ -n "$pid" ]]; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
        python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$scratch"
    }
    trap cleanup_training EXIT
    cp "$SEED_DB" "$scratch/feed.db" || return 1
    PORT=0 HOST=127.0.0.1 JWT_SECRET=twelve-dollar-challenge SQLITE_PATH="$scratch/feed.db" \
        "$SERVER" >"$scratch/server.log" 2>&1 &
    pid=$!
    local port=
    for ((attempt = 0; attempt < 100; attempt++)); do
        kill -0 "$pid" 2>/dev/null || return 1
        port=$(sed -n 's/.*Server listening on port \([0-9]*\) with.*/\1/p' "$scratch/server.log")
        if [[ -n "$port" ]] && curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null; then
            break
        fi
        port=
        sleep 0.05
    done
    [[ -n "$port" ]] || return 1
    local token base="http://127.0.0.1:$port"
    token=$(python3 - <<'TOKEN'
import base64
import hashlib
import hmac
import json
import time

def encode(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")

header = encode(b'{"alg":"HS256","typ":"JWT"}')
claims = encode(json.dumps({"sub": 1, "username": "alice", "exp": int(time.time()) + 3600},
                           separators=(",", ":")).encode())
message = header + b"." + claims
signature = hmac.digest(b"twelve-dollar-challenge", message, hashlib.sha256)
print((message + b"." + encode(signature)).decode())
TOKEN
    ) || return 1
    local -a http=(curl -fsS --max-time 2)
    for ((i = 0; i < 40; i++)); do
        "${http[@]}" "$base/feed" "$base/health" "$base/posts/1" >/dev/null || return 1
        "${http[@]}" -X POST -H "Authorization: Bearer $token" "$base/posts/1/like" >/dev/null || return 1
        "${http[@]}" -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
            -d '{"body":"PGO training post"}' "$base/posts" >/dev/null || return 1
    done
    kill -TERM "$pid" || return 1
    wait "$pid" || return 1
    pid=
    [[ -n "$(find "$PGO_DIR" -type f -name '*.gcda' -print -quit)" ]] || return 1
)

if [[ -f "$SEED_DB" ]]; then
    echo "==> Training the instrumented server on a scratch copy of the seed..."
    if gcc "${common[@]}" -fprofile-generate="$PGO_DIR" -o "$SERVER" "${sources[@]}" && train_profile; then
        profile=(-fprofile-use="$PGO_DIR" -fprofile-correction -Werror=missing-profile)
    else
        echo "==> Profile training failed. Building without profile feedback." >&2
    fi
fi

echo "==> Linking the production server..."
if (( ${#profile[@]} )) &&
    gcc "${common[@]}" "${final[@]}" "${profile[@]}" -o "$SERVER" "${sources[@]}"; then
    echo "==> Applied profile feedback."
else
    gcc "${common[@]}" "${final[@]}" -o "$SERVER" "${sources[@]}"
fi
mv "$SERVER" "$DIR/bin/server"

cp "$DIR/build/stage1" "$DIR/bin/stage1"
echo "==> Build complete! Outputs: bin/server, bin/stage1"
