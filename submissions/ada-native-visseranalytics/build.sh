#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin obj
case $(uname -m) in
  x86_64) event_offset=4; event_bytes=12; event_alignment=4 ;;
  aarch64) event_offset=8; event_bytes=16; event_alignment=8 ;;
  *) echo 'Unsupported Linux architecture' >&2; exit 1 ;;
esac
cat > obj/platform.ads <<EOF
package Platform is
   Epoll_Data_Offset : constant := $event_offset;
   Epoll_Event_Bytes : constant := $event_bytes;
   Epoll_Event_Alignment : constant := $event_alignment;
end Platform;
EOF
exec gnatmake-13 -j1 -O2 -gnat2022 -gnatwa -gnatVa -fstack-protector-strong \
  -D obj -Iobj -Isrc src/server.adb -o bin/server -largs -lsqlite3 -lcrypto
