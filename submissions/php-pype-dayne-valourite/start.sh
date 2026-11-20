#!/usr/bin/env bash
# Runs the server in the foreground: pype on OpenSwoole, serving HOST:PORT itself (direct, no Nginx).
# memory_limit=-1: in base mode OpenSwoole counts each connection's 64 KB receive buffer against it,
# so a finite limit kills the worker at a few thousand idle keep-alive connections (RSS stays ~600 MB).
# Reads SQLITE_PATH, JWT_SECRET, HOST and PORT from the environment (the app's .env only fills gaps).
set -euo pipefail
cd "$(dirname "$0")"
# OpenSwoole settings (config/app.php reads every SWOOLE_* variable). Base mode: the one worker
# accepts and serves its connections itself, no reactor-to-worker hop (~20% less CPU per request).
export SWOOLE_MODE=base SWOOLE_BACKLOG=8192
exec php \
  -d extension=openswoole \
  -d opcache.enable_cli=1 \
  -d opcache.validate_timestamps=0 \
  -d opcache.jit=tracing \
  -d opcache.jit_buffer_size=64M \
  -d memory_limit=-1 \
  bin/pype serve --server=swoole --host="${HOST:-0.0.0.0}" --port="${PORT:-80}" --workers=1
