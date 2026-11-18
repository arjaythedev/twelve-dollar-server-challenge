#!/usr/bin/env bash
# Runs the server in the foreground. Configuration: SQLITE_PATH, JWT_SECRET, HOST, PORT (SPEC.md).
set -euo pipefail
# GC off, 1 GiB soft limit as the only trigger: the request path allocates nothing, and the other
# half of the 2 GB box belongs to the kernel (60k sockets) and the mmap'd database in page cache.
export GOGC="${GOGC:-off}" GOMEMLIMIT="${GOMEMLIMIT:-1GiB}"
exec "$(dirname "$0")/bin/server"
