#!/usr/bin/env bash
set -euo pipefail
if [ ! -f /data/feed.db ]; then
  bash /challenge/seed/make-seed.sh
  cp /challenge/seed/feed.db /data/feed.db.tmp
  cp /challenge/seed/tokens.json /data/tokens.json.tmp
  mv /data/tokens.json.tmp /data/tokens.json
  mv /data/feed.db.tmp /data/feed.db
fi
chown -R 10001:10001 /data
