#!/usr/bin/env bash
# Runs the server in the foreground: Flycast boots the Dreamcast program (build/dreamcast-sql.elf)
# with the HLE BIOS and an emulated Broadband Adapter. Our Flycast patches read:
#   HOSTFS_PATH          the host file the guest's SQLite VFS reads and writes ($SQLITE_PATH)
#   FLYCAST_TCP_FORWARD  host ports forwarded to the same ports on the Dreamcast ($PORT)
#   FLYCAST_TCP_BIND     the host address they listen on ($HOST)
# The guest reads JWT_SECRET and PORT through the same syscall. Without a display (the scoring
# box), Flycast draws the request dashboard into its own Xvfb; DREAMCAST_SCREEN=1 skips that.
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
B="${DREAMCAST_BUILD:-$D/build}"
: "${SQLITE_PATH:?}" "${JWT_SECRET:?}"
export PORT="${PORT:-3000}" HOST="${HOST:-0.0.0.0}"
# A real SH-4 runs at 200 MHz. Flycast will run it faster, and the server is bound by it.
# The guest reads this too, for its dashboard.
export SH4_CLOCK="${SH4_CLOCK:-3200}"

run="$(mktemp -d)"
mkdir -p "$run/flycast"
cat > "$run/flycast/emu.cfg" << EOF
[config]
UseReios = yes
Sh4Clock = $SH4_CLOCK
Dreamcast.RamMod32MB = yes
Dynarec.Enabled = yes
Dreamcast.AutoLoadState = no
Dreamcast.AutoSaveState = no
PerGameVmu = no
Debug.SerialConsoleEnabled = yes

[audio]
backend = null

[network]
EmulateBBA = yes
DCNet = no
EnableUPnP = no
DNS = 127.0.0.1
EOF

export XDG_CONFIG_HOME="$run" XDG_DATA_HOME="$run"
export HOSTFS_PATH="$SQLITE_PATH" FLYCAST_TCP_FORWARD="$PORT" FLYCAST_TCP_BIND="$HOST"

cmd=("$B/flycast/flycast" "$B/dreamcast-sql.elf")
xpid=0
child=0
wdog=0
cleanup() {
  kill "$wdog" "$child" 2> /dev/null; wait "$child" 2> /dev/null
  [ "$xpid" = 0 ] || kill "$xpid" 2> /dev/null
  rm -rf "$run"
}
trap 'cleanup; exit 0' TERM INT
if [ "$(uname)" = Linux ] && [ -z "${DISPLAY:-}" ] && [ "${DREAMCAST_SCREEN:-0}" != 1 ]; then
  Xvfb -displayfd 3 -nolisten tcp -screen 0 640x480x24 3> "$run/display" > /dev/null 2>&1 &
  xpid=$!
  for _ in $(seq 50); do [ -s "$run/display" ] && break; sleep 0.1; done
  export DISPLAY=":$(cat "$run/display")"
fi

# A safety net: if Flycast exits, a real Dreamcast would get its power button pressed; so does
# this one. Committed rows are already in the host file (the WAL is written through), so a
# reboot loses only the requests in flight. Same if the guest's main loop stops: it logs
# "heartbeat" every 5 s (swallowed here). /health would be the wrong probe, since under
# overload it waits in line behind everyone else and a busy Dreamcast is not a stuck one.
heartbeats() {
  local line
  while IFS= read -r line; do
    if [ "$line" = "[dreamcast] heartbeat" ]; then echo "$SECONDS" > "$run/heartbeat"; else printf '%s\n' "$line"; fi
  done
}
watchdog() {
  local hb
  while sleep 5 && kill -0 "$1" 2> /dev/null; do
    hb="$(cat "$run/heartbeat" 2> /dev/null)"
    if [ -n "$hb" ] && [ $((SECONDS - hb)) -gt 30 ]; then
      echo "dreamcast-sql: no heartbeat for 30 s, rebooting the Dreamcast" >&2
      kill "$1" 2> /dev/null; sleep 3; kill -9 "$1" 2> /dev/null
      return
    fi
  done
}
while :; do
  echo "$((SECONDS + 30))" > "$run/heartbeat" # 30 s more to boot
  "${cmd[@]}" > >(heartbeats) &
  child=$!
  watchdog "$child" &
  wdog=$!
  wait "$child" || true
  kill "$wdog" 2> /dev/null || true
  echo "dreamcast-sql: Flycast exited, rebooting the Dreamcast" >&2
  sleep 1
done
