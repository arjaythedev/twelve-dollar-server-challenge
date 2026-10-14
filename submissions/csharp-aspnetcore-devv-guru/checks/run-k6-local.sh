#!/usr/bin/env bash
set -euo pipefail
pid_file="${1:?pid file required}"
shift
ulimit -n 65535
printf '%s\n' "$$" > "$pid_file"
exec "$@"
