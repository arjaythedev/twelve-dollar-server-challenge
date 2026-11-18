#!/usr/bin/env bash
# Where does the event loop sleep? Starts a stress run, then records off-CPU kernel+user stacks of the
# server's event-loop thread with offcputime-bpfcc (needs root: perf_event_paranoid=4 on Ubuntu). Output
# (folded stacks, microseconds off CPU per stack) goes to results/offcpu-<time>.txt; the run itself is
# recorded as usual.
#
#   sudo -v && dev/offcpu.sh [N]
#
# Env: GEN (blast, default, N = connections; or users, N = users), DUR (run length; 40s for blast, 1m for
# users), AFTER (seconds after the load generator starts; default 10), SECS (how long to record; default 15),
# plus anything dev/ct.sh takes (DATA, ...).
# Example, the users ramp-up stall:  sudo -v && GEN=users AFTER=15 SECS=30 dev/offcpu.sh 200000
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-1024}"
GEN="${GEN:-blast}"
[[ "$N" =~ ^[0-9]+$ ]] || { echo "usage: dev/offcpu.sh [N]  (got '$N': must be a number)" >&2; exit 2; }
[[ "$GEN" = blast || "$GEN" = users ]] || { echo "GEN must be blast or users (got '$GEN')" >&2; exit 2; }
AFTER="${AFTER:-10}"; SECS="${SECS:-15}"
[ "$GEN" = users ] && DUR="${DUR:-1m}" || DUR="${DUR:-40s}"
out="$HERE/results/offcpu-$(date +%Y%m%d-%H%M%S).txt"
command -v offcputime-bpfcc >/dev/null || { echo "needs offcputime-bpfcc (apt install bpfcc-tools)" >&2; exit 1; }
sudo -n true 2>/dev/null || { echo "run 'sudo -v' first (this script uses sudo for offcputime-bpfcc)" >&2; exit 1; }

GEN="$GEN" LABEL="probe: offcpu" NOTES="offcputime profile +${AFTER}s for ${SECS}s" "$HERE/dev/ct.sh" stress "$N" "$DUR" &
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
sleep "$AFTER"
echo "profiling julia pid $pid (event-loop thread = pid) for $SECS s -> ${out#"$HERE"/}"
sudo offcputime-bpfcc -f -t "$pid" "$SECS" > "$out" 2> "$out.err" || true
[ -s "$out" ] || { echo "offcputime recorded nothing; its stderr:" >&2; cat "$out.err" >&2; }
wait "$runner" || true
total=$(awk '{s+=$NF} END {print s+0}' "$out")
echo "off-CPU total: $((total / 1000)) ms in $SECS s. Top stacks (us off CPU, last 4 frames):"
awk '{v=$NF; $NF=""; n=split($0, f, ";"); printf "%10d  %s;%s;%s;%s\n", v, f[n-3], f[n-2], f[n-1], f[n]}' "$out" |
  sort -nr | head -10
