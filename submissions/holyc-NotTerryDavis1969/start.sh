#!/usr/bin/env bash
# HolyC $12 Server Challenge - start.sh
# Runs the server in the foreground, configured only by the env vars in SPEC.md
set -euo pipefail

# Configuration from environment variables (as specified in SPEC.md)
export JWT_SECRET=twelve-dollar-challenge
export HOST=127.0.0.1
export PORT=3000
export SQLITE_PATH="/feed.db"

# Verify the server binary exists
if [ ! -x "./server" ]; then
    echo "Error: server binary not found. Run build.sh first."
    exit 1
fi

# Run the server in the foreground
exec ./server