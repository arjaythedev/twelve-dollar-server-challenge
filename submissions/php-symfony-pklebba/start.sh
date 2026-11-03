#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
export APP_ROOT="$PWD" HOST="${HOST:-127.0.0.1}" PORT="${PORT:-3000}"
export XDG_CONFIG_HOME="$PWD/var/caddy" XDG_DATA_HOME="$PWD/var/caddy"
# Recommended by the FrankenPHP performance guide: skip cgo pointer checks on every PHP<->Go call.
export GODEBUG=cgocheck=0
exec frankenphp run --config Caddyfile
