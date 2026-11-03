#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# If the server is killed (it volunteers for the OOM killer under extreme overload), start it again so
# the next run finds it up; committed writes are in the WAL. Any other exit (e.g. bad config) is final.
cd "$(dirname "$0")"
while true; do
  status=0
  bin/server || status=$?
  [ "$status" -ge 128 ] || exit "$status"
  echo "server killed (status $status), restarting" >&2
  sleep 1
done
