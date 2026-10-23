#!/usr/bin/env bash
# Runs OpenResty in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# Every path nginx writes to (pid, temp dirs, rendered config) lives in run/, so it works as a non-root user.
set -euo pipefail
cd "$(dirname "$0")"
: "${SQLITE_PATH:?}" "${JWT_SECRET:?}" "${HOST:=0.0.0.0}" "${PORT:=80}"
mkdir -p run
{
  # started as root, nginx would drop its worker to "nobody", which can't write SQLITE_PATH
  [ "$(id -u)" = 0 ] && echo "user root;"
  sed -e "s|__LISTEN__|$HOST:$PORT|" -e "s|__ROOT__|$PWD|g" nginx.conf
} > run/nginx.conf
exec /usr/local/openresty/bin/openresty -p "$PWD/run/" -c "$PWD/run/nginx.conf" -e stderr
