# Pinned versions, sourced by install.sh, build.sh and start.sh. Change versions here only.
JULIA_VERSION=1.13.3
JULIA_SHA256=5c61cb4b40332cb5a3504e7779e2f5dac093caa4a1116ded19f01413f85c63e4
JULIA_URL="https://julialang-s3.julialang.org/bin/linux/x64/${JULIA_VERSION%.*}/julia-${JULIA_VERSION}-linux-x86_64.tar.gz"
JULIA_HOME="/opt/julia-${JULIA_VERSION}"

SQLITE_VERSION=3.54.1
SQLITE_ID=3540100
SQLITE_YEAR=2026
SQLITE_SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30   # as published on sqlite.org
SQLITE_URL="https://sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_ID}.zip"

# Read path (PLAN.md phase 5b): sql (public SQLite API, default) or btree (not implemented yet).
READ_BACKEND=sql

# GC soft target for the Julia heap (julia --heap-size-hint). The box has 2 GB for everything.
JULIA_HEAP_HINT=512M
