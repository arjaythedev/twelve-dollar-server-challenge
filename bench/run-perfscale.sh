#!/usr/bin/env bash
# Example:
#   PERFSCALE_BIN=/path/to/perfscale VUS=1000 DURATION=5m RAMP_UP=60s bash bench/run-perfscale.sh
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
PERFSCALE_BIN="${PERFSCALE_BIN:-perfscale}"
BASE_URL="${BASE_URL:-http://127.0.0.1:3000}"
VUS="${VUS:-100}"
DURATION="${DURATION:-5m}"
RAMP_UP="${RAMP_UP:-60s}"
RAMP_DOWN="${RAMP_DOWN:-30s}"
TOKENS="${TOKENS:-$ROOT/seed/tokens.json}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

sed \
  -e "s/__VUS__/$VUS/g" \
  -e "s/__DURATION__/$DURATION/g" \
  -e "s/__RAMP_UP__/$RAMP_UP/g" \
  -e "s/__RAMP_DOWN__/$RAMP_DOWN/g" \
  "$DIR/perfscale.config.template.yaml" > "$TMP/config.yaml"

export BASE_URL TOKENS
"$PERFSCALE_BIN" run -q -f "$DIR/perfscale.test.yaml" -c "$TMP/config.yaml" "$@"
