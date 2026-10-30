#!/usr/bin/env bash
# Reproduce the native transaction/read checks on independent seed copies.
set -euo pipefail
cd "$(dirname "$0")"
repo="$(cd ../.. && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
flags=(-O2 -march=x86-64-v3)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
fi
"${CC:-cc}" "${flags[@]}" -Wno-deprecated-declarations -pthread \
  -I.deps -I.deps/sqlite -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 \
  verify_batch.c bin/yyjson.o bin/sqlite.o -lcrypto -lm -ldl -o "$scratch/verify-batch"
cp "$repo/seed/feed.db" "$scratch/batch.db"
token="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["token"])' "$repo/seed/tokens.json")"
"$scratch/verify-batch" "$scratch/batch.db" "$token"
"${CC:-cc}" "${flags[@]}" -pthread -I.deps/sqlite \
  verify_native.c bin/sqlite.o -lm -ldl -o "$scratch/verify-native"
cp "$repo/seed/feed.db" "$scratch/native.db"
"$scratch/verify-native" "$scratch/native.db"
