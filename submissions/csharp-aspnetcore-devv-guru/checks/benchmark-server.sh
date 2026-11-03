#!/usr/bin/env bash
set -euo pipefail
seed="${1:?seed path required}"
[ ! -e "$SQLITE_PATH" ] || { echo 'benchmark database already exists' >&2; exit 1; }
# Copy inside the cgroup so database page cache is charged to the server limit.
cp -- "$seed" "$SQLITE_PATH"
# Keep runtime assemblies on ext4 as well as the database for local WSL tests.
publish="${2:-$(cd "$(dirname "$0")/../bin/publish" && pwd)}"
run_directory="$(dirname "$SQLITE_PATH")"
cp -a -- "$publish" "$run_directory/app"
cd "$run_directory/app"
exec ./FeedApi
