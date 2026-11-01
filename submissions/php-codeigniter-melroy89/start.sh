#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${SQLITE_PATH:?SQLITE_PATH is required}" "${JWT_SECRET:?JWT_SECRET is required}"
export HOST="${HOST:-127.0.0.1}" PORT="${PORT:-3000}"
export CI_ENVIRONMENT=production APP_STARTED_AT="$(date +%s)" APP_ROOT="$PWD"
php -r '$p=getenv("SQLITE_PATH"); if (!is_file($p)) { throw new RuntimeException("database not found"); } $db=new PDO("sqlite:".$p); $db->exec("PRAGMA journal_mode=WAL");'
export GODEBUG=cgocheck=0 GOMAXPROCS=1 GOMEMLIMIT=768MiB
exec frankenphp run --config "$PWD/Caddyfile" --adapter caddyfile
