#!/usr/bin/env bash
# Runs the server in the foreground: one process, one worker on HOST:PORT,
# which holds every connection as an io_uring direct descriptor, past
# LimitNOFILE, and the database alone.
set -euo pipefail
cd "$(dirname "$0")"
exec bin/server
