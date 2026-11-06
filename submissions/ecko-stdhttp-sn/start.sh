#!/usr/bin/env bash
# start.sh: runs the server in the foreground. Config comes from SQLITE_PATH, JWT_SECRET, HOST, PORT.
# Served directly (no Nginx): every load-test user holds its own keep-alive connection. Connections are
# kept for 75 s, so a user's next request reuses its connection instead of paying a TCP handshake, and
# the server keeps at most 55,000 open: at about 9 KB each (0.80.0) that is about 500 MB of the 2 GB
# box, and it stays below the box's 65,535 file descriptors. Past that, Ecko (0.77+) closes the connection idle longest to make room for a newcomer, and that
# user reconnects when it next asks for something.
cd "$(dirname "$0")"
# Each connection is a file descriptor. The benchmark box gives the service LimitNOFILE=65535, but a
# login shell's soft limit is often 1,024, which caps the server near 1,000 users. Raise our own soft
# limit to match the box (or as far as the hard limit allows); it needs no root.
ulimit -n 65535 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
export ECKO_HTTP_MAX_CONNS="${ECKO_HTTP_MAX_CONNS:-55000}"
export ECKO_HTTP_READ_TIMEOUT_MS="${ECKO_HTTP_READ_TIMEOUT_MS:-75000}"
exec "${ECKO:-ecko}" server.ecko
