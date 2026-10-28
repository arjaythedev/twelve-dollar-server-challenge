#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"
ulimit -Sn "$(ulimit -Hn)" 2>/dev/null || true
export PATH="/opt/ruby/bin:$PATH"
export RUBY_YJIT_ENABLE=1
# Compile after 5 calls instead of 30, so paths the startup warm-up doesn't cover (error responses,
# HTTP/1.0 clients) run compiled code almost at once.
exec bundle exec ruby --yjit --yjit-call-threshold=5 server.rb
