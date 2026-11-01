#!/usr/bin/env bash
# start.sh: runs the server in the foreground. Configured only by the env vars
# in SPEC.md: SQLITE_PATH, JWT_SECRET, HOST, PORT.
set -euo pipefail
cd "$(dirname "$0")"

# Raise the open-file limit to the box's ceiling. In direct mode every keep-alive
# client is an fd (the SPEC allows up to ~15,000), and the benchmark box grants
# LimitNOFILE=65535. Without this the default soft limit (often 1024) caps
# concurrent connections far below the pool and accept() starts failing EMFILE.
# Raise the soft limit up to the hard limit (best effort; ignore if not allowed).
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

# gforth loads the layers in order and calls epoll-main (the epoll event loop).
# `exec` so the server is PID-visible to the supervisor and receives signals
# directly.
exec gforth server.fs
