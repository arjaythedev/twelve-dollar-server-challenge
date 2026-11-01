#!/usr/bin/env bash
# Tried and not kept: the experiments in local-bench/experiments/, each a folder with one of
#   Caddyfile / Caddyfile.classic  replaces the app's (worker mode / classic mode)
#   php.ini                        added to the app's php.d/
#   env                            environment variables for the server (one VAR=value per line, # comments)
#   patch                          applied to a copy of the app (diff -u, paths from the app folder), built as its own image
#
#   bash local-bench/experiment.sh setup <experiment> [classic]
#       prints the image and the `docker run` args for bench.sh start, building the image of a patch experiment
#   bash local-bench/experiment.sh compare <users> <rounds> [classic] <experiment>...
#       the app (bench.sh build's image) against each experiment, throttled like the ladder (0.25 CPU, saturated)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
IMAGE="${IMAGE:-twelve-dollar-php-symfony}"
THROTTLE="${THROTTLE:---cpus=0.25}"

setup() {
  local name="$1" mode="${2:-worker}" dir="$HERE/experiments/$1" image="$IMAGE" args="$THROTTLE"
  [ -d "$dir" ] || { echo "no such experiment: $name" >&2; exit 1; }
  if [ "$mode" = classic ]; then
    if [ -f "$dir/Caddyfile.classic" ]; then args="$args -v $dir/Caddyfile.classic:/app/Caddyfile:ro"
    else args="$args -v $HERE/Caddyfile.classic:/app/Caddyfile:ro"; fi
  elif [ -f "$dir/Caddyfile" ]; then args="$args -v $dir/Caddyfile:/app/Caddyfile:ro"
  elif [ -f "$dir/Caddyfile.classic" ]; then echo "$name is a classic mode experiment" >&2; exit 1
  fi
  [ -f "$dir/php.ini" ] && args="$args -v $dir/php.ini:/app/php.d/zz-experiment.ini:ro"
  if [ -f "$dir/env" ]; then
    while IFS= read -r line; do case "$line" in ''|\#*) ;; *) args="$args -e $line" ;; esac; done < "$dir/env"
  fi
  if [ -f "$dir/patch" ]; then
    image="$IMAGE:exp-$name"
    local src; src="$(mktemp -d)"
    # the committed app, as bench.sh build-peer does for other submissions (from the top: run in a subfolder,
    # git archive only keeps the tree's paths under that subfolder)
    git -C "$(git -C "$APP" rev-parse --show-toplevel)" archive "HEAD:$(git -C "$APP" rev-parse --show-prefix)" | tar -x -C "$src"
    patch -s -f -d "$src" -p1 < "$dir/patch" || { echo "$name: the patch doesn't apply" >&2; exit 1; }
    docker build -q -f "$src/local-bench/Dockerfile" -t "$image" "$src" >/dev/null || { echo "$name: the image doesn't build" >&2; exit 1; }
    rm -rf "$src"
  fi
  echo "$image" "$args"
}

case "${1:-}" in
  setup) setup "${2:?experiment}" "${3:-worker}" ;;
  compare)
    users="${2:?users}"; rounds="${3:?rounds}"; shift 3
    mode=worker; if [ "${1:-}" = classic ]; then mode=classic; shift; fi
    base_args="$THROTTLE"; [ "$mode" = classic ] && base_args="$base_args -v $HERE/Caddyfile.classic:/app/Caddyfile:ro"
    setups=("$mode-app" "$IMAGE" "$base_args")
    for name in "$@"; do
      setup="$(setup "$name" "$mode")" || exit 1
      read -r image args <<< "$setup"
      setups+=("$mode-$name" "$image" "$args")
    done
    WARMUP=0 DURATION="${DURATION:-2m}" RAMP_UP="${RAMP_UP:-30s}" bash "$HERE/compare.sh" "$users" "$rounds" "${setups[@]}"
    ;;
  *) sed -n '2,12p' "$0"; exit 1 ;;
esac
