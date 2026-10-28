#!/usr/bin/env bash
# Reproducible workstation diagnostics, not an official capacity evaluation.
set -euo pipefail
cd "$(dirname "$0")"
ADA=$(pwd)
ROOT=$(cd ../.. && pwd)
if [[ -d /Applications/Docker.app/Contents/Resources/bin ]]; then
  export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi
ID=$(printf '%s' "$ADA" | shasum -a 256 | cut -c1-12)
PROJECT="ada-local-$ID"
export ADA_LOCAL_IMAGE="$PROJECT:workstation"
compose() { docker compose --project-name "$PROJECT" --file "$ADA/compose.local.yaml" "$@"; }
fail() { printf '%s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'HELP'
Usage: bash submissions/ada-native-visseranalytics/local.sh doctor|test|up|load|load-sharded|stats|logs|down
  doctor  Check Docker, seed files and VM resources.
  test    Build and run all correctness suites in a disposable container.
  up      Build direct Ada and start http://127.0.0.1:3001 (ADA_SERVE_MODE=nginx for proxy).
  load    Run the unchanged k6 script (1,000 VUs, 30s hold by default).
  load-sharded  Synchronized execution segments; K6_SHARDS=2 by default.
                CPU/RAM/GOMEMLIMIT overrides apply PER SHARD.
  stats   Show live CPU/RAM use of this checkout's server and load generators.
  logs    Show server logs.
  down    Stop this checkout's containers and remove its private DB volume.
  cost    Isolated request CPU diagnostic; run outside load tests.
  generator-status  Inspect only this checkout's active k6 control APIs.
  restart-preflight Read-only inventory before a separately approved Docker restart.
  runtime-info Read-only server process, emulator and resource-limit details.

Overrides: ADA_LOCAL_PORT=3001 VUS=1000 DURATION=30s RAMP_UP=60s
           K6_CPUS=4 K6_MEMORY_GB=4 K6_GOMEMLIMIT=3GiB
Use test --reuse-image to reuse the cached pinned toolchain and rebuild current sources.
Use up --reuse-image to rebuild with the cached toolchain and runtime dependencies.
Use load-sharded --help for explicit per-generator budgets and execution segments.
Run down then up to reset the database before a new comparison.
HELP
}
ACTION=${1:-help}
case "$ACTION" in
  help|-h|--help) usage; exit 0 ;;
  doctor|test|up|cost|restart-preflight|runtime-info|load|load-sharded|generator-status|stats|logs|down) ;;
  *) usage; exit 2 ;;
esac
if ! docker info --format '{{.NCPU}} {{.MemTotal}} {{.Architecture}}' > /dev/null; then
  fail 'Docker is unavailable to this shell. Start Docker Desktop and run here from a terminal with Docker access.'
fi
docker compose version >/dev/null
case "$ACTION" in
  doctor)
    docker info --format 'Docker VM: {{.NCPU}} CPUs; {{.MemTotal}} bytes RAM; {{.Architecture}}'
    for f in seed/feed.db seed/tokens.json bench/load.js bench/nginx.conf; do
      test -s "$ROOT/$f" || fail "Missing $f"
    done
    printf 'Seed and benchmark files present. Project: %s\n' "$PROJECT"
    printf 'Server: 1 CPU / 2 GiB. Default k6: 4 CPUs / 4 GiB. Allow VM/other-container headroom.\n'
    ;;
  test)
    if [[ ${2:-} == --reuse-image ]]; then export ADA_VALIDATION_REUSE_IMAGE=1; fi
    exec bash "$ADA/validate-container.sh"
    ;;
  load-sharded) shift; exec python3 "$ADA/bench/local_load.py" "$PROJECT" "$@" ;;
  runtime-info)
    compose exec -T --user 10001:10001 server python3 -c '
import json, os
from pathlib import Path
pid = int(Path("/run/ada/app.pid").read_text())
proc = Path("/proc") / str(pid)
maps = (proc / "maps").read_text().splitlines()
print(json.dumps(dict(pid=pid, executable=os.readlink(proc / "exe"),
    emulator_mappings=[line for line in maps if "rosetta" in line.lower() or "qemu" in line.lower()],
    fd_limit=[line for line in (proc / "limits").read_text().splitlines() if "Max open files" in line],
    cgroup={name: (Path("/sys/fs/cgroup") / name).read_text().strip()
            for name in ("cpu.max", "memory.max", "memory.swap.max")}), indent=2))'
    ;;
  restart-preflight)
    # Read-only inventory for a separately approved Docker resource change.
    for container in $(docker ps --quiet); do
      docker inspect "$container" --format '{"name":{{json .Name}},"id":{{json .Id}},"auto_remove":{{json .HostConfig.AutoRemove}},"restart":{{json .HostConfig.RestartPolicy}},"mounts":{{json .Mounts}}}'
    done
    ;;
  cost)
    docker run --rm --platform linux/amd64 --cpus=1 --memory=2g --memory-swap=2g \
      --mount "type=bind,source=$ROOT,target=/input,readonly" \
      --workdir /work "ada-validation:$ID" bash -euc '
        cp -R /input/submissions/ada-native-visseranalytics/src .
        cp /input/submissions/ada-native-visseranalytics/bench/request_cost.adb .
        cp /input/seed/feed.db .
        mkdir obj
        gnatmake-13 -j1 -O2 -gnat2022 -gnatwa -gnatVa -fstack-protector-strong \
          -D obj -Isrc request_cost.adb -o request_cost -largs -lsqlite3 -lcrypto
        export SQLITE_PATH=/work/feed.db
        for repeat in 1 2 3; do
          for mode in headers feed post wire; do ./request_cost "$mode" 50000; done
        done
      '
    ;;
  generator-status)
    for container in $(docker ps --quiet --filter "label=com.docker.compose.project=$PROJECT" --filter "label=com.docker.compose.service=load"); do
      docker exec "$container" k6 status --address 127.0.0.1:6565
    done
    ;;
  up)
    test -s "$ROOT/seed/feed.db" || fail 'Missing seed/feed.db'
    REVISION=$(git rev-parse --verify HEAD 2>/dev/null || printf 'unknown')
    # This label deliberately identifies a worktree build, including uncommitted edits.
    if [[ ${2:-} == --reuse-image ]]; then
      docker build --platform linux/amd64 --file Dockerfile.cached \
        --build-arg "TOOLCHAIN_IMAGE=ada-validation:$ID" \
        --build-arg "RUNTIME_IMAGE=$ADA_LOCAL_IMAGE" --tag "$ADA_LOCAL_IMAGE" .
    else
      docker build --platform linux/amd64 --build-arg "REVISION=$REVISION-worktree" --tag "$ADA_LOCAL_IMAGE" .
    fi
    compose up --detach server
    ready=0
    for ((attempt=0; attempt<60; attempt++)); do
      if compose exec -T server curl -fsS http://127.0.0.1/health; then ready=1; break; fi
      sleep 1
    done
    if [[ $ready == 0 ]]; then compose logs server; fail 'Server did not become healthy within 60 seconds.'; fi
    printf '\nReady: http://127.0.0.1:%s (database retained until local.sh down)\n' "${ADA_LOCAL_PORT:-3001}"
    ;;
  load)
    VUS=${VUS:-1000}
    K6_MEMORY_GB=${K6_MEMORY_GB:-4}
    [[ $VUS =~ ^[1-9][0-9]*$ ]] || fail 'VUS must be a positive integer.'
    [[ $K6_MEMORY_GB =~ ^[1-9][0-9]*$ ]] || fail 'K6_MEMORY_GB must be a positive integer.'
    (( K6_MEMORY_GB >= 2 )) || fail 'Use at least 2 GiB for the load generator.'
    export K6_MEMORY_GB
    export K6_GOMEMLIMIT=${K6_GOMEMLIMIT:-$((K6_MEMORY_GB - 1))GiB}
    VM_MEMORY=$(docker info --format '{{.MemTotal}}')
    (( VM_MEMORY >= (K6_MEMORY_GB + 3) * 1073741824 )) || fail 'Docker VM needs server + k6 budgets plus at least 1 GiB headroom. Check Docker Desktop Resources.'
    ARCH=$(docker info --format '{{.Architecture}}')
    case "$ARCH" in
      aarch64|arm64) export ADA_LOAD_PLATFORM=linux/arm64 ;;
      x86_64|amd64) export ADA_LOAD_PLATFORM=linux/amd64 ;;
      *) fail "Unsupported Docker architecture: $ARCH" ;;
    esac
    SERVER=$(compose ps --quiet server)
    test -n "$SERVER" || fail 'Start the server with local.sh up first.'
    compose exec -T server curl -fsS http://127.0.0.1/health >/dev/null
    test -s "$ROOT/seed/tokens.json" || fail 'Missing seed/tokens.json'
    RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-vu$VUS-$$"
    export ADA_LOAD_RESULTS="$ADA/benchmark-results/local/$RUN_ID"
    mkdir -p "$ADA_LOAD_RESULTS"
    printf '{}\n' > "$ADA_LOAD_RESULTS/k6-config.json"
    compose --profile load config > "$ADA_LOAD_RESULTS/compose.yaml"
    shasum -a 256 src/* "$ROOT/seed/feed.db" "$ROOT/seed/tokens.json" \
      "$ROOT/bench/load.js" "$ROOT/bench/nginx.conf" > "$ADA_LOAD_RESULTS/sha256.txt"
    docker inspect "$SERVER" > "$ADA_LOAD_RESULTS/server.json"
    JOB="$PROJECT-load-$RUN_ID"
    STATS_PID=
    # shellcheck disable=SC2329 # Invoked by EXIT/INT/TERM traps.
    cleanup_load() {
      trap - EXIT INT TERM
      if [[ -n $STATS_PID ]]; then kill "$STATS_PID" 2>/dev/null || true; wait "$STATS_PID" 2>/dev/null || true; fi
      # The unique name belongs only to this invocation, including interrupted runs.
      if docker inspect "$JOB" >/dev/null 2>&1; then
        docker stop --time 10 "$JOB" >/dev/null 2>&1 || true
        docker logs "$JOB" > "$ADA_LOAD_RESULTS/console.log" 2>&1 || true
        docker inspect "$JOB" > "$ADA_LOAD_RESULTS/loadgen.json" || true
        docker inspect "$SERVER" > "$ADA_LOAD_RESULTS/server-after.json" || true
        docker rm "$JOB" >/dev/null || true
      fi
    }
    trap cleanup_load EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    compose run --detach --no-deps --name "$JOB" load run --quiet \
      --config /results/k6-config.json --summary-export /results/summary.json \
      --out json=/results/samples.json.gz \
      -e BASE_URL=http://server -e TOKENS=/seed/tokens.json \
      -e "VUS=$VUS" -e "RAMP_UP=${RAMP_UP:-60s}" -e "DURATION=${DURATION:-30s}" /scripts/load.js
    # Streaming docker stats emits terminal escapes even when redirected.
    # Poll snapshots so the retained file is valid JSONL, with sample timestamps.
    (
      trap 'exit 0' TERM INT
      while :; do
        docker stats --no-stream \
          --format "{\"sampled_at\":\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\",\"stats\":{{json .}}}" \
          "$SERVER" "$JOB"
        sleep 3
      done
    ) > "$ADA_LOAD_RESULTS/stats.jsonl" 2> "$ADA_LOAD_RESULTS/stats.err" &
    STATS_PID=$!
    printf 'Running %s VUs. Results: %s\n' "$VUS" "$ADA_LOAD_RESULTS"
    RC=$(docker wait "$JOB")
    printf '%s\n' "$RC" > "$ADA_LOAD_RESULTS/exit.txt"
    docker logs "$JOB"
    docker inspect "$JOB" --format 'Load generator: exit={{.State.ExitCode}} OOM={{.State.OOMKilled}}'
    printf 'Whole-run statistics include ramps; this is a local diagnostic, not an official score.\n'
    exit "$RC"
    ;;
  stats)
    CONTAINERS=$(docker ps --quiet --filter "label=com.docker.compose.project=$PROJECT")
    test -n "$CONTAINERS" || fail 'No running local containers.'
    # IDs from Docker contain no whitespace except the intentional word separators.
    # shellcheck disable=SC2086
    docker stats $CONTAINERS
    ;;
  logs) compose logs --tail 100 server ;;
  down) compose --profile load down --volumes ;;
esac
