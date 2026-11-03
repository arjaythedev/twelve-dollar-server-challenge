#!/usr/bin/env bash
# Runs the server in the foreground, configured only by SQLITE_PATH, JWT_SECRET,
# HOST and PORT. Port 80 needs CAP_NET_BIND_SERVICE from the service unit.
set -euo pipefail
exec "$(dirname "$0")/bin/feed"
