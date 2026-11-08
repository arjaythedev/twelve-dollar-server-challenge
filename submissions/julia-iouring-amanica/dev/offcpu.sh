#!/usr/bin/env bash
# Where does the event loop sleep at saturation? (PLAN 4.1: ~25% idle while clients wait.)
# Starts a blast run, then records off-CPU kernel+user stacks of the server's event-loop thread for
# 15 s with offcputime-bpfcc (needs root: perf_event_paranoid=4 on Ubuntu). Output (folded stacks,
# microseconds off CPU per stack) goes to results/offcpu-<time>.txt; the run itself is recorded as usual.
#
#   sudo -v && dev/offcpu.sh [connections]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-1024}"
[[ "$N" =~ ^[0-9]+$ ]] || { echo "usage: dev/offcpu.sh [connections]  (got '$N': must be a number)" >&2; exit 2; }
out="$HERE/results/offcpu-$(date +%Y%m%d-%H%M%S).txt"
command -v offcputime-bpfcc >/dev/null || { echo "needs offcputime-bpfcc (apt install bpfcc-tools)" >&2; exit 1; }
sudo -n true 2>/dev/null || { echo "run 'sudo -v' first (this script uses sudo for offcputime-bpfcc)" >&2; exit 1; }

GEN=blast LABEL="probe: 4.1 offcpu" NOTES="offcputime profile" "$HERE/dev/ct.sh" stress "$N" 40s &
runner=$!
pid=""
name="jr-srv-$runner-$N"                    # dev/ct.sh names its server jr-srv-<its pid>-<N>
for _ in $(seq 180); do                    # wait until this run's server and load generator are up
  podman container exists "$name" && podman ps --format '{{.Names}}' | grep -q "^jr-gen-$runner-" && \
    pid="$(podman inspect -f '{{.State.Pid}}' "$name")" && break
  kill -0 "$runner" 2>/dev/null || break
  sleep 1
done
[ -n "$pid" ] || { echo "server or generator did not start" >&2; wait "$runner"; exit 1; }
sleep 10                                    # past the warm-up, into saturation
echo "profiling julia pid $pid (event-loop thread = pid) for 15 s -> ${out#"$HERE"/}"
sudo offcputime-bpfcc -f -t "$pid" 15 > "$out" 2> "$out.err" || true
[ -s "$out" ] || { echo "offcputime recorded nothing; its stderr:" >&2; cat "$out.err" >&2; }
wait "$runner" || true
total=$(awk '{s+=$NF} END {print s+0}' "$out")
echo "off-CPU total: $((total / 1000)) ms in 15 s. Top stacks (us off CPU, last 4 frames):"
awk '{v=$NF; $NF=""; n=split($0, f, ";"); printf "%10d  %s;%s;%s;%s\n", v, f[n-3], f[n-2], f[n-1], f[n]}' "$out" |
  sort -nr | head -10
