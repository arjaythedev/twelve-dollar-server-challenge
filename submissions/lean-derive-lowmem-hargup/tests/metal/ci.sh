#!/usr/bin/env bash
# Invoke from the committed source mirror on bigmachine, with no concurrent benchmark.
set -euo pipefail
sha=${1:?full source commit SHA}
root=/root/twelve/opt-audit-1024/metal
out=$root/results/ci-${sha:0:7}-$(date +%s)
mkdir -p "$out"
printf '%s\n' "$sha" > "$out/source.sha"
docker image inspect ubuntu:24.04 --format '{{.Id}}' > "$out/image.txt"
# io_uring requires this seccomp exception; no privileged container or host namespace.
rc=0
docker run --rm --name "metal-ci-${sha:0:7}-$$" \
 --security-opt seccomp=unconfined --cpuset-cpus=0 --memory=2g --memory-swap=2g \
 --ulimit nofile=65535:65535 --ulimit memlock=8388608:8388608 \
 --mount "type=bind,source=$root/source,target=/input,readonly" \
 --mount type=bind,source=/root/twelve/challenge/test,target=/tests,readonly \
 --mount type=bind,source=/root/twelve/challenge/seed,target=/seed,readonly \
 ubuntu:24.04 bash /input/tests/metal/ci-inner.sh > "$out/ci.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$out/exit-code.txt"
tail -55 "$out/ci.log"
printf 'CI_RESULTS %s\n' "$out"
exit "$rc"
