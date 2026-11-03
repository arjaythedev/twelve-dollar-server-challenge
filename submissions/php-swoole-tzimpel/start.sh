#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT (WORKERS optional, default 1).
set -euo pipefail
cd "$(dirname "$0")"
exec php -c php.ini -d extension="$PWD/build/swoole.so" server.php
