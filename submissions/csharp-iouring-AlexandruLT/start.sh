#!/usr/bin/env bash
set -euo pipefail
# Use the published 65,535 descriptor limit even when the launcher inherited a lower soft
# limit (one ring's 60,000 registered slots must fit). Never request a larger hard limit.
hard_nofile=$(ulimit -Hn)
if [ "$hard_nofile" = unlimited ] || [ "$hard_nofile" -ge 65535 ]; then
  ulimit -Sn 65535
  ulimit -Hn 65535
fi
dir="$(dirname "$0")/bin"
export LD_LIBRARY_PATH="$dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$dir/Feed"
