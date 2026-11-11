#!/usr/bin/env bash
# Local capacity diagnostic: 11 workers in one client namespace, one server CPU.
set -euo pipefail
cd "$(dirname "$0")/../.."
submission="$PWD/submissions/rust-iouring-rkkautsar"
run_dir=$(mktemp -d /tmp/feed-240k.XXXXXX)
name="feed240k-$$"
link_prefix="v$$"
hold_seconds=${HOLD_SECONDS:-30}
target_users=${USERS:-240000}
server_shards=${SHARDS:-$(((target_users + 61999) / 62000))}
verify_connections=${VERIFY_CONNECTIONS:-0}
memory_limit_mb=${BENCH_MEMORY_LIMIT_MB:-6144}
min_available_mb=${MIN_HOST_AVAILABLE_MB:-2048}
cpu_limit=${BENCH_CPU_LIMIT:-8}
cpu_count=$(nproc)
if ! [[ "$cpu_limit" =~ ^[1-9][0-9]*$ ]] || (( cpu_limit > cpu_count - 2 )); then
  echo 'BENCH_CPU_LIMIT must leave at least two host CPUs available' >&2
  exit 1
fi
for value in "$memory_limit_mb" "$min_available_mb"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'Safety limits must be positive integer MiB' >&2; exit 1; }
done
available_kb=$(grep '^MemAvailable:' /proc/meminfo | { read -r _ value _; echo "$value"; })
if (( available_kb < min_available_mb * 1024 )); then
  echo "FAIL: host available memory is below ${min_available_mb} MiB" >&2
  exit 1
fi
cgroup_name="feedbench-$$"
cgroup_path="/sys/fs/cgroup/$cgroup_name"
docker image inspect lgipimg:latest >/dev/null
docker run --rm --privileged --cgroupns host lgipimg:latest sh -c '
  set -eu
  d="/sys/fs/cgroup/$1"
  mkdir "$d"
  trap '\''rmdir "$d" 2>/dev/null || true'\'' EXIT
  echo "$2" >"$d/memory.max"
  echo 0 >"$d/memory.swap.max"
  echo 1 >"$d/memory.oom.group"
  echo "$3 100000" >"$d/cpu.max"
  echo "+memory +cpu" >"$d/cgroup.subtree_control"
  trap - EXIT
' -- "$cgroup_name" "$((memory_limit_mb * 1024 * 1024))" "$((cpu_limit * 100000))"
containers=()
cleanup() {
  if [ -n "${monitor_pid:-}" ]; then kill "$monitor_pid" 2>/dev/null || true; fi
  if [ -n "${pressure_pid:-}" ]; then kill "$pressure_pid" 2>/dev/null || true; fi
  if [ -n "${reset_pid:-}" ]; then kill "$reset_pid" 2>/dev/null || true; fi
  if [ -n "${watchdog_pid:-}" ]; then kill "$watchdog_pid" 2>/dev/null || true; fi
  if [ -n "${observer_pid:-}" ]; then kill "$observer_pid" 2>/dev/null || true; fi
  docker rm -f "$name-observer" >/dev/null 2>&1 || true
  docker inspect "$name-server" >"$run_dir/server-inspect.json" 2>&1 || true
  docker logs "$name-server" >"$run_dir/server.log" 2>&1 || true
  docker inspect "$name-client" >"$run_dir/client-inspect.json" 2>&1 || true
  docker rm -f "$name-server" "${containers[@]}" >/dev/null 2>&1 || true
  docker run --rm --privileged --cgroupns host lgipimg:latest \
    cat "$cgroup_path/memory.events" >"$run_dir/cgroup-memory.events" 2>&1 || true
  docker run --rm --privileged --cgroupns host lgipimg:latest \
    cat "$cgroup_path/cpu.stat" >"$run_dir/cgroup-cpu.stat" 2>&1 || true
  docker run --rm --privileged --cgroupns host lgipimg:latest \
    cat "$cgroup_path/memory.peak" >"$run_dir/cgroup-memory.peak" 2>&1 || true
  docker run --rm --privileged --cgroupns host lgipimg:latest \
    rmdir "$cgroup_path" 2>/dev/null || echo "Warning: could not remove $cgroup_path" >&2
}
trap cleanup EXIT
printf 'Results: %s\n' "$run_dir"
mkdir -p "$run_dir/data" "$run_dir/results"
cp seed/feed.db "$run_dir/data/feed.db"
chmod 777 "$run_dir/results"

docker run -d --name "$name-server" --label "feedbench.run=$name" --cgroup-parent "/$cgroup_name" --network none --cpuset-cpus=11 \
  --memory=1536m --memory-swap=1536m --security-opt seccomp=unconfined \
  --ulimit nofile=1048576:1048576 -e JWT_SECRET=twelve-dollar-challenge \
  -e SQLITE_PATH=/data/feed.db -e HOST=0.0.0.0 -e PORT=3000 -e SHARDS="$server_shards" -e STATS="${STATS:-}" -e FEED_BUDGET="${FEED_BUDGET:-}" -e POST_BUDGET="${POST_BUDGET:-}" -e WAL_AUTOCHECKPOINT="${WAL_AUTOCHECKPOINT:-}" -e CHECKPOINT_EVERY_MS="${CHECKPOINT_EVERY_MS:-}" \
  -v "$submission/target/release/feed-server:/server:ro" -v "$run_dir/data:/data" \
  ubuntu:24.04 /server >"$run_dir/server-id"
for _ in $(seq 1 30); do
  if docker exec "$name-server" bash -c 'exec 3<>/dev/tcp/127.0.0.1/3000; printf "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n" >&3; read -r status <&3; [[ "$status" == "HTTP/1.1 200 "* ]]' 2>/dev/null; then break; fi
  if [ "$(docker inspect --format '{{.State.Running}}' "$name-server")" != true ]; then
    echo 'Server exited before becoming healthy' >&2
    exit 1
  fi
  sleep 1
done
docker exec "$name-server" bash -c 'exec 3<>/dev/tcp/127.0.0.1/3000; printf "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n" >&3; read -r status <&3; [[ "$status" == "HTTP/1.1 200 "* ]]'
server_pid=$(docker inspect -f '{{.State.Pid}}' "$name-server")
docker exec "$name-server" sh -c '
  while :; do
    date +%s
    cat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.stat /sys/fs/cgroup/memory.events
    sleep 5
  done
' >"$run_dir/memory.log" 2>&1 &
monitor_pid=$!
(
  while :; do
    awk -F'avg10=' '/^some/{print "PSI_MEM", $2+0; exit}' /proc/pressure/memory 2>/dev/null
    awk -F'avg10=' '/^some/{print "PSI_CPU", $2+0; exit}' /proc/pressure/cpu 2>/dev/null
    awk '/^cpu /{print "STEAL", $9}' /proc/stat
    sleep 2
  done
) >"$run_dir/pressure.log" 2>&1 &
pressure_pid=$!
start_ms=$(( $(date +%s%3N) + 60000 ))
container="$name-client"
containers+=("$container")
docker run -d --name "$container" --label "feedbench.run=$name" --cgroup-parent "/$cgroup_name" --network none --privileged --cpuset-cpus=0-10 \
  --sysctl 'net.ipv4.ip_local_port_range=1024 65535' --ulimit nofile=1048576:1048576 \
  -e LG_START_UNIX_MS="$start_ms" -e LG_PLATEAU_DELAY_S=75 -e LG_RAMPDOWN_S=30 \
  -e TARGET_USERS="$target_users" -e HOLD_SECONDS="$hold_seconds" \
  -v "$submission/target/release/loadgen:/loadgen:ro" \
  -v "$PWD/seed/tokens.json:/tokens.json:ro" -v "$run_dir/results:/results" \
  ubuntu:24.04 /bin/bash -c '
    set -eu
    pids=()
    for i in $(seq 0 10); do
      while [ ! -e "/sys/class/net/direct$i/queues/rx-0/rps_cpus" ]; do sleep .1; done
      printf "%x\n" "$((1 << i))" >"/sys/class/net/direct$i/queues/rx-0/rps_cpus"
      count=$((TARGET_USERS / 11 + (i < TARGET_USERS % 11 ? 1 : 0)))
      offset=$((i * (TARGET_USERS / 11) + (i < TARGET_USERS % 11 ? i : TARGET_USERS % 11)))
      (
        rc=0
        LG_HISTOGRAM_PATH="/results/$i.json" LG_VU_OFFSET="$offset" \
          taskset -c "$i" /loadgen "10.77.$i.1" 3000 "$count" 60 "$HOLD_SECONDS" /tokens.json 0 600 "$i" >"/results/$i.log" 2>&1 || rc=$?
        echo "$rc" >"/results/$i.exit"
        exit "$rc"
      ) &
      pids+=("$!")
    done
    rc=0
    for pid in "${pids[@]}"; do
      if wait "$pid"; then
        :
      else
        status=$?
        # 99 means the worker wrote a complete result but failed its local
        # thresholds; the aggregate histogram decides the run thresholds.
        [ "$status" -eq 99 ] || rc=1
      fi
    done
    exit "$rc"
  ' >/dev/null
client_pid=$(docker inspect -f '{{.State.Pid}}' "$container")
# Create only temporary veth pairs; links moved into the disposable namespaces
# disappear with their containers. The helper needs iproute2 and nsenter.
docker run --rm --label "feedbench.run=$name" --cgroup-parent "/$cgroup_name" --privileged --pid host --network host lgipimg:latest sh -c '
set -eu
server=$1; client=$2; prefix=$3
cleanup_links() { for j in $(seq 0 10); do ip link del "${prefix}a$j" 2>/dev/null || true; ip link del "${prefix}b$j" 2>/dev/null || true; done; }
trap cleanup_links EXIT
for i in $(seq 0 10); do
  a="${prefix}a$i"; b="${prefix}b$i"
  ip link add "$a" type veth peer name "$b"
  ip link set "$a" netns "$server"
  ip link set "$b" netns "$client"
  nsenter -t "$server" -n ip address add "10.77.$i.1/30" dev "$a"
  nsenter -t "$server" -n ip link set "$a" up
  nsenter -t "$client" -n ip link set "$b" name "direct$i"
  nsenter -t "$client" -n ip address add "10.77.$i.2/30" dev "direct$i"
  nsenter -t "$client" -n ip link set "direct$i" up
done
' -- "$server_pid" "$client_pid" "$link_prefix"
(
  target=$((start_ms + 75000))
  while [ "$(date +%s%3N)" -lt "$target" ]; do sleep 1; done
  docker exec "$name-server" kill -USR1 1 >/dev/null 2>&1 || true
) &
reset_pid=$!
(
  docker wait "$name-server" >"$run_dir/server-exit.tmp"
  mv "$run_dir/server-exit.tmp" "$run_dir/server-exit"
  echo 'Server exited during the load test' >&2
  docker kill "${containers[@]}" >/dev/null 2>&1 || true
) &
watchdog_pid=$!
if [ "$verify_connections" = 1 ]; then
  (
    hold_start_ms=$((start_ms + 75000))
    hold_end_ms=$((hold_start_ms + hold_seconds * 1000))
    delay=$(((hold_start_ms + 2000 - $(date +%s%3N) + 999) / 1000))
    if [ "$delay" -gt 0 ]; then sleep "$delay"; fi
    sample=0
    while [ "$(date +%s%3N)" -lt "$((hold_end_ms - 15000))" ]; do
      docker run --rm --name "$name-observer" --label "feedbench.run=$name" --cgroup-parent "/$cgroup_name" --privileged --pid host --network host --cpuset-cpus=0 \
        -v "$run_dir/results:/results" lgipimg:latest sh -c '
          set -eu
          dir="/results/snapshot-$1"; server=$2; client=$3
          mkdir -p "$dir"
          date +%s%3N >"$dir/start"
          nsenter -t "$server" -n ss -H4tn state established >"$dir/server"
          nsenter -t "$client" -n ss -H4tn state established >"$dir/client"
          date +%s%3N >"$dir/end"
        ' -- "$sample" "$server_pid" "$client_pid"
      sample=$((sample + 1))
      remaining=$(((hold_end_ms - $(date +%s%3N) + 999) / 1000))
      if [ "$remaining" -le 0 ]; then break; fi
      if [ "$remaining" -gt 50 ]; then remaining=50; fi
      sleep "$remaining"
    done
  ) >"$run_dir/observer.log" 2>&1 &
  observer_pid=$!
fi
docker wait "$container" >"$run_dir/client-exit"
if [ -n "${observer_pid:-}" ]; then wait "$observer_pid"; observer_pid=; fi
if [ -e "$run_dir/server-exit" ] || [ "$(docker inspect --format '{{.State.Running}}' "$name-server")" != true ]; then
  echo 'FAIL: server exited before benchmark completion' >&2
  exit 1
fi
set +e
python3 - "$run_dir/results" "$hold_seconds" "$target_users" "$verify_connections" "$start_ms" "$(cat "$run_dir/client-exit")" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
files = sorted(p.glob('*.json'))
if len(files) != 11:
    raise SystemExit(f'FAIL: {len(files)}/11 workers produced results')
workers = [json.loads(f.read_text()) for f in files]
hist = [sum(row) for row in zip(*(w['histogram_100us'] for w in workers))]
total = sum(hist)
def pct(q):
    target = (total * q + 99) // 100
    count = 0
    for i, n in enumerate(hist):
        count += n
        if count >= target:
            return (i + 1) / 10
    return float('inf')
target = sum(w['target'] for w in workers)
minimum = sum(w['min_active'] for w in workers)
requests = sum(w['requests'] for w in workers)
errors = sum(w['errors'] for w in workers)
p95, p99 = pct(95), pct(99)
passed = sys.argv[6] == '0' and target == int(sys.argv[3]) and all(w['reached'] and w['min_active'] >= w['target'] for w in workers) and requests > 0 and total > 0 and errors / requests < .01 and p95 < 500 and p99 < 1000
if sys.argv[4] == '1':
    hold_start = int(sys.argv[5]) + 75000
    hold_end = hold_start + int(sys.argv[2]) * 1000
    valid = 0
    for snapshot in sorted(p.glob('snapshot-*')):
        start, end = (int((snapshot / name).read_text()) for name in ('start', 'end'))
        if start < hold_start or end > hold_end:
            print(f'{snapshot.name}: outside hold window ({start}..{end})')
            continue
        def pairs(path, reverse=False):
            rows = [line.split()[-2:] for line in path.read_text().splitlines()]
            if any(len(row) != 2 for row in rows):
                raise SystemExit(f'FAIL: malformed socket snapshot {path}')
            return [(remote, local) if reverse else (local, remote) for local, remote in rows]
        server = pairs(snapshot / 'server')
        clients = pairs(snapshot / 'client', True)
        server_set, client_set = set(server), set(clients)
        matched = (len(server) == target == len(server_set)
                   and len(clients) == target == len(client_set) and server_set == client_set)
        print(f'{snapshot.name}: server={len(server)} clients={len(clients)} matched={matched} duration_ms={end-start}')
        passed &= matched
        valid += 1
    if not valid:
        print('FAIL: no complete kernel socket snapshot inside the hold window')
        passed = False
print(f'{"PASS" if passed else "FAIL"} hold_s={sys.argv[2]} target={target} min_active_sum={minimum} requests={requests} errors={errors} p95_ms={p95:.1f} p99_ms={p99:.1f}')
import pathlib
_lag = pathlib.Path(sys.argv[1]) / 'clientlag.txt'
_pq = max(w["packet_queue_lag_p99_ms"] for w in workers)
_tl = max(w["timer_lag_p99_ms"] for w in workers)
_lag.write_text(f'{_pq} {_tl}')
print(f'client worst_worker_timer_lag_p99_ms={max(w["timer_lag_p99_ms"] for w in workers)} worst_worker_read_batch_lag_p99_ms={max(w["read_batch_lag_p99_ms"] for w in workers)} worst_worker_packet_queue_lag_p99_ms={max(w["packet_queue_lag_p99_ms"] for w in workers)} packet_samples={sum(w["packet_queue_samples"] for w in workers)} (1000 means >=1000)')
sys.exit(0 if passed else 1)
PY
gate_status=$?
set -e
if [ -s "$run_dir/pressure.log" ]; then
  psi_mem_max=$(awk '$1=="PSI_MEM"{if($2>m)m=$2} END{printf "%g", m+0}' "$run_dir/pressure.log")
  psi_cpu_max=$(awk '$1=="PSI_CPU"{if($2>m)m=$2} END{printf "%g", m+0}' "$run_dir/pressure.log")
  steal_max=$(awk '$1=="STEAL"{if($2>m)m=$2} END{print m+0}' "$run_dir/pressure.log")
  lag_pq=0; lag_tl=0
  if [ -s "$run_dir/results/clientlag.txt" ]; then
    read -r lag_pq lag_tl < "$run_dir/results/clientlag.txt"
  fi
  if awk -v v="$lag_pq" 'BEGIN{exit !(v>30)}'; then
    echo "GATE INVALID reason=client_lag packet_queue=${lag_pq}ms timer=${lag_tl}ms"
  elif [ "$gate_status" -ne 0 ]; then
    echo "GATE INVALID reason=latency"
  elif awk -v v="$psi_mem_max" -v lim="${MAX_PSI_MEM:-2.0}" 'BEGIN{exit !(v>lim)}'; then
    echo "GATE INVALID reason=memory_pressure psi_mem=$psi_mem_max"
  elif [ "$steal_max" -gt 0 ]; then
    echo "GATE INVALID reason=host_steal steal=$steal_max"
  else
    echo "GATE VALID psi_mem=$psi_mem_max psi_cpu=$psi_cpu_max"
  fi
else
  echo "GATE INVALID reason=no_pressure_samples"
fi
if [ -e "$run_dir/server-exit" ] || [ "$(docker inspect --format '{{.State.Running}}' "$name-server")" != true ]; then
  echo 'FAIL: server exited before benchmark completion' >&2
  exit 1
fi
