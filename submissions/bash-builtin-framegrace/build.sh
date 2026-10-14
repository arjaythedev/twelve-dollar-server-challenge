#!/usr/bin/env bash
# build.sh: runs as a normal user
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
chmod +x "$DIR/start.sh" "$DIR/worker.sh"
gcc -shared -fPIC -O3 "$DIR/hmac_b64.c" -lcrypto -o "$DIR/hmac_b64.so"
