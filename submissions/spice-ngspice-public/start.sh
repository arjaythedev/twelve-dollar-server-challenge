#!/usr/bin/env bash
# start.sh: socat on HOST:PORT, one wire/conn.sh per connection, in the foreground.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${SQLITE_PATH:?}" "${JWT_SECRET:?}" "${HOST:=127.0.0.1}" "${PORT:=3000}"
export BOOT_FILE=$(mktemp "${TMPDIR:-/tmp}/spice-boot.XXXXXX")
date +%s > "$BOOT_FILE"
# nodelay: the response leaves bash in several writes; without it Nagle + delayed ACK add ~40 ms
exec socat "TCP-LISTEN:$PORT,bind=$HOST,reuseaddr,fork,backlog=1024,nodelay" EXEC:"$here/wire/conn.sh"
