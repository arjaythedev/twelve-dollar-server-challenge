#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
exec php \
  -d extension=swoole \
  -d memory_limit=-1 \
  -d opcache.enable_cli=1 \
  -d opcache.jit=tracing \
  -d opcache.jit_buffer_size=32M \
  server.php
