#!/usr/bin/env bash
# Run the challenge suite or benchmark in a disposable, native-architecture Compose project.
#   scripts/docker.sh test
#   scripts/docker.sh benchmark [benchmark.py options]
# BUILD_IMAGES=false reuses images from make image. BENCH_K6=local runs k6 on
# this host against the published port instead of in a container.
set -euo pipefail
action=${1:?usage: docker.sh test|benchmark [benchmark.py options]}
shift
here=$(cd "$(dirname "$0")/.." && pwd)
DOCKER_DEFAULT_PLATFORM=$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')
export DOCKER_DEFAULT_PLATFORM
export LOCAL_PORT=0
project="twelve-dollar-java-$action-$$-$RANDOM"
compose=(docker compose -p "$project" -f "$here/compose.yaml")
work=$(mktemp -d "${TMPDIR:-/tmp}/twelve-dollar-java.XXXXXX")
child=
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/scripts/lib.sh"

cleanup() {
  local status=$?
  trap - EXIT
  if ! "${compose[@]}" --profile test down --volumes --remove-orphans >"$work/cleanup.log" 2>&1; then
    cat "$work/cleanup.log" >&2
    status=2
  fi
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

if [ "${BUILD_IMAGES:-true}" = true ]; then
  "${compose[@]}" --profile test build server seed test
fi
"${compose[@]}" up --no-build -d --wait --wait-timeout "${WAIT_TIMEOUT:-120}"

case "$action" in
  test)
    "${compose[@]}" --profile test run --rm --no-deps test
    ;;
  benchmark)
    port=$("${compose[@]}" port server 80)
    url="http://127.0.0.1:${port##*:}"
    docker cp "$("${compose[@]}" ps -aq seed):/data/tokens.json" "$work/tokens.json"
    options=(--mode docker --url "$url" --tokens "$work/tokens.json"
      --app-exec "docker exec $("${compose[@]}" ps -q server)")
    if [ "${PROFILE:-false}" = true ]; then
      options+=(--jfr-exec "docker exec $("${compose[@]}" ps -q server)")
    fi
    case "${BENCH_K6:-docker}" in
      docker) options+=(--k6 docker --k6-url http://server --docker-network "${project}_default") ;;
      local)
        ensure_k6
        raise_file_limit
        options+=(--k6 local --k6-bin "$K6_BIN")
        ;;
      *) echo "BENCH_K6 must be docker or local" >&2; exit 2 ;;
    esac
    python3 "$here/benchmark.py" "${options[@]}" "$@" &
    child=$!
    wait "$child"
    ;;
  *)
    echo "Unknown action: $action" >&2
    exit 2
    ;;
esac
