#!/usr/bin/env bash
# Runs the server in the foreground. Config comes from SQLITE_PATH, JWT_SECRET, HOST and PORT.
# No restart loop: if the server exits, the supervisor sees it.
set -euo pipefail
cd "$(dirname "$0")"

# The runtime posture the design rests on. The local rig set these in its compose file, so they
# were in every measurement while the box got the Go defaults - GOMAXPROCS=NumCPU, GOGC=100.
#
# GOMAXPROCS=1: one thread owns the ring, the connection table and SQLite (SQLITE_THREADSAFE=0).
# GOGC=off: the serving path allocates nothing per request and the arena holds no pointers.
#   GOMEMLIMIT is the backstop that collects anyway if that stops being true.
#
# Defaults, not overrides: the rig varies them to measure what they are worth.
export GOMAXPROCS="${GOMAXPROCS:-1}"
export GOGC="${GOGC:-off}"
export GOMEMLIMIT="${GOMEMLIMIT:-1400MiB}"

exec ./bin/server
