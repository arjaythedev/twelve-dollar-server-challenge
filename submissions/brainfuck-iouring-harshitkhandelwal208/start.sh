#!/usr/bin/env bash
set -euo pipefail
export PORT="${PORT:-80}"
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/bin/server"
