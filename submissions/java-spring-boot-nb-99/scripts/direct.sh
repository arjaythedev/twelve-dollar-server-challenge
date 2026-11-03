#!/usr/bin/env bash
# Run the challenge suite or benchmark against the app on this host.
#   scripts/direct.sh test
#   scripts/direct.sh benchmark [benchmark.py options]
# The app serves k6 directly, as on the challenge VPS. Uses installed Java 25 and
# k6 v2 when present. Missing tools are downloaded to a temporary directory that
# is deleted afterwards.
set -euo pipefail
action=${1:?usage: direct.sh test|benchmark [benchmark.py options]}
shift
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/twelve-dollar-java.XXXXXX")
pids=()
child=
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/scripts/lib.sh"

cleanup() {
  local status=$?
  trap - EXIT
  for pid in ${pids[@]+"${pids[@]}"}; do kill "$pid" 2>/dev/null || true; done
  for pid in ${pids[@]+"${pids[@]}"}; do wait "$pid" 2>/dev/null || true; done
  rm -rf "$work"
  exit "$status"
}
trap cleanup EXIT
# Bash runs traps only after a foreground command exits, so the benchmark runs in
# the background. Ctrl-C already reaches it; SIGTERM sent to make must be forwarded.
stop() {
  if [ -n "$child" ]; then
    [ "$1" = INT ] || kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  exit "$2"
}
trap 'stop INT 130' INT
trap 'stop TERM 143' TERM

ensure_java() {
  JAVA=${JAVA:-java}
  local major
  major=$("$JAVA" -version 2>&1 | awk -F'"' '/version/ { split($2, v, "."); print v[1]; exit }') || true
  if [ "${major:-0}" -ge 25 ] 2>/dev/null; then return; fi
  local name sha
  case "$platform" in
    Darwin/arm64) name=aarch64_mac sha=964ffce1ff9f24e8d29df4e4201b5b96499026dc8387cf35bb39fe969ffea2bb ;;
    Darwin/x86_64) name=x64_mac sha=720829aa4e4bddf3e95171cc059a52f7b9c56f443313dfef05114701bf60da06 ;;
    Linux/x86_64) name=x64_linux sha=8daf77d1aacffe38c9889689bc224a13557de77559d9a5bb91991e6a298baa0d ;;
    Linux/aarch64 | Linux/arm64) name=aarch64_linux sha=5c83b7d2121ed482fd06831a1eba1633dbab818aba6addddf48e075b97e6e9b8 ;;
    *) echo "No Java 25 download for $platform" >&2; exit 1 ;;
  esac
  echo "Java 25 not found; downloading Temurin 25.0.1 to $work"
  fetch "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.1%2B8/OpenJDK25U-jdk_${name}_hotspot_25.0.1_8.tar.gz" "$sha" "$work/jdk.tar.gz"
  mkdir "$work/jdk"
  tar -xzf "$work/jdk.tar.gz" -C "$work/jdk" --strip-components=1
  JAVA=$work/jdk/bin/java
  [ -x "$JAVA" ] || JAVA=$work/jdk/Contents/Home/bin/java
}

[ -f "$here/target/server.jar" ] || { echo "target/server.jar is missing; run make build" >&2; exit 1; }
need curl
need python3
ensure_java

if [ ! -f "$root/seed/feed.db" ] || [ ! -f "$root/seed/tokens.json" ]; then
  need node
  need sqlite3
  bash "$root/seed/make-seed.sh"
fi
cp "$root/seed/feed.db" "$work/feed.db"

[ "${PROFILE:-false}" = false ] || { echo "PROFILE=true needs MODE=docker or MODE=k8s" >&2; exit 2; }
# The app holds one keep-alive connection per k6 user, so both need many open files.
raise_file_limit
port=$(free_port)
url=http://127.0.0.1:$port
JAVA=$JAVA SQLITE_PATH=$work/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=$port \
  bash "$here/start.sh" >"$work/app.log" 2>&1 &
app_pid=$!
pids+=("$app_pid")
if ! wait_healthy "$url" "$app_pid"; then
  echo "The app did not become healthy. Last log lines:" >&2
  tail -n 40 "$work/app.log" >&2
  exit 1
fi

case "$action" in
  test)
    need jq
    need openssl
    NO_PROXY='*' no_proxy='*' bash "$root/test/test.sh" "$url"
    ;;
  benchmark)
    ensure_k6
    python3 "$here/benchmark.py" --mode direct --url "$url" \
      --k6 local --k6-bin "$K6_BIN" --tokens "$root/seed/tokens.json" --app-pid "$app_pid" "$@" &
    child=$!
    wait "$child"
    ;;
  *)
    echo "Unknown action: $action" >&2
    exit 2
    ;;
esac
