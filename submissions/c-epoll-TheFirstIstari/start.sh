#!/usr/bin/env bash
# Runs the server in the foreground, configured only by SQLITE_PATH,
# JWT_SECRET, HOST and PORT. Restarts if the kernel OOM-kills it.
cd "$(dirname "$0")"
while true; do
  ./bin/server
  rc=$?
  echo "server exited ($rc), restarting in 1s" >&2
  sleep 1
done
