#!/usr/bin/env bash
# Memory snapshot of the running server (bench.sh start, with Caddyfile.worker-pprof for Go's numbers):
#
#   SERVE_ARGS="-v $PWD/local-bench/profile/Caddyfile.worker-pprof:/app/Caddyfile:ro" bash local-bench/bench.sh start
#   bash local-bench/profile/memory.sh <out-dir>
#
# Writes the cgroup's memory.stat, the server's RSS, socket and fd counts, Go's MemStats, a heap profile (read it with
# `go tool pprof -top heap.pb.gz`) and goroutine counts. No full goroutine dump (debug=2): near 2 GB, the server
# building that 64 MB text was enough to get it OOM-killed.
set -euo pipefail
out="${1:?usage: memory.sh <out-dir>}"
mkdir -p "$out"
docker exec twelve-dollar-server sh -c '
  cd /tmp
  cat /sys/fs/cgroup/memory.current > m.current
  cat /sys/fs/cgroup/memory.stat > m.stat
  cat /proc/1/smaps_rollup > m.smaps_rollup
  grep -E "VmRSS|RssAnon|RssFile|RssShmem|Threads" /proc/1/status > m.status
  cat /proc/net/sockstat > m.sockstat
  ls /proc/1/fd | wc -l > m.fds
  curl -s "localhost:2019/debug/pprof/heap?debug=1" | grep -E "^# [A-Z]" > m.memstats
  curl -s -o m.heap.pb.gz localhost:2019/debug/pprof/heap
  curl -s "localhost:2019/debug/pprof/goroutine?debug=1" | grep -E "^(goroutine profile|[0-9]+ @)" > m.goroutines
'
for f in current stat smaps_rollup status sockstat fds memstats heap.pb.gz goroutines; do
  docker cp -q "twelve-dollar-server:/tmp/m.$f" "$out/$f"
done
