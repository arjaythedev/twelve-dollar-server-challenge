#!/usr/bin/env bash
# Build and start the simulated server (fresh DB copy each start), then wait for /health.
set -euo pipefail
cd "$(dirname "$0")"
docker compose up -d --build --force-recreate
PORT="${HOST_PORT:-8080}"
for _ in $(seq 60); do curl -sf "http://127.0.0.1:$PORT/health" && echo && exit 0; sleep 1; done
echo "not healthy after 60 s"; docker compose logs --tail 30; exit 1
