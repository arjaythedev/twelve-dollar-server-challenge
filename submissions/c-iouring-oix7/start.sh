#!/usr/bin/env bash
# Launch the server in the foreground on 0.0.0.0:80 (requires CAP_NET_BIND_SERVICE or root).
# No kernel tuning: the server runs on the box exactly as provisioned (challenge rule 11).
set -euo pipefail

exec "$(dirname "$0")/bin/server"
