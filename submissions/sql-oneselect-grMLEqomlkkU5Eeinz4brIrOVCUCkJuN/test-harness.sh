#!/usr/bin/env bash
# Runs the challenge's own test/test.sh with no network: its curl calls are swapped for bin/harness,
# which pushes the same HTTP bytes through the parser, the virtual table and server.sql.
#
#   ./build.sh harness && ./test-harness.sh
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$DIR/../.." && pwd)"
export JWT_SECRET=twelve-dollar-challenge SQLITE_PATH="$(mktemp -d)/feed.db"
trap 'rm -rf "$(dirname "$SQLITE_PATH")" "$DIR/.test.sh"' EXIT
cp "$ROOT/seed/feed.db" "$SQLITE_PATH"
REQ='req() {
  { printf "%s %s HTTP/1.1\r\nHost: x\r\n" "$1" "$2"
    [ -n "${3:-}" ] && [ "$3" != "-" ] && printf "Authorization: %s\r\n" "$3"
    if [ -n "${4+x}" ]; then printf "Content-Type: application/json\r\nContent-Length: %s\r\n\r\n%s" "$(printf "%s" "$4" | wc -c)" "$4"
    else printf "\r\n"; fi; } | "'"$DIR"'/bin/harness" "'"$DIR"'/src/server.sql" > "$TMP/raw"
  STATUS="$(head -1 "$TMP/raw" | cut -d" " -f2)"
  CTYPE="$(grep -ai "^content-type:" "$TMP/raw" | head -1 | cut -d" " -f2- | tr -d "\r")"
  BODY="$(sed "1,/^\r$/d" "$TMP/raw")"
}'
awk -v req="$REQ" '/^req\(\) \{/{print req; skip=1; next} skip&&/^\}/{skip=0; next} !skip' "$ROOT/test/test.sh" > "$DIR/.test.sh"
sed -i "s|^GOLDEN=.*|GOLDEN=\"$ROOT/test/golden\"|" "$DIR/.test.sh"
bash "$DIR/.test.sh" harness
