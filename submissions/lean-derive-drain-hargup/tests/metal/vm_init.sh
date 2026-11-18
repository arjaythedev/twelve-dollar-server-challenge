#!/bin/bash
# vm_init.sh — PID 1 of the 1-vCPU / 2 GB benchmark guest (vm153.py). Kernel command line:
# variant=NAME. Brings up eth0 as 10.153.0.2/16, starts /variants/NAME/server the way the host
# harness does (UID 65534, LimitNOFILE 65535, HOST=0.0.0.0 PORT=3000, the seed database at
# /work/feed.db: the disk is a throwaway qemu -snapshot, so every boot starts from the seed), then
# writes one line per second to /dev/hvc0:
#   @S <uptime> | <cpu line of /proc/stat> | <server /proc/PID/stat after the comm> | <MemAvailable kB>
#      <server VmRSS kB> | <TCP line of /proc/net/sockstat>
# The loop uses shell builtins only (no fork per sample). Server output -> /dev/hvc1, kernel log ->
# /dev/hvc2 (both are files on the host).
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
mkdir -p /dev/pts /dev/shm; mount -t devpts devpts /dev/pts; mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp; mount -t tmpfs tmpfs /run; mount -t cgroup2 none /sys/fs/cgroup
exec > /dev/hvc0 2>&1
V=; PA=; PF=; PR=; for w in $(< /proc/cmdline); do case $w in variant=*) V=${w#variant=} ;; perf_at=*) PA=${w#perf_at=} ;; perf_for=*) PF=${w#perf_for=} ;; perf_report_at=*) PR=${w#perf_report_at=} ;; esac; done
# What Ubuntu 24.04's boot sets before any service starts (this init replaces systemd): systemd
# raises fs.file-max to LONG_MAX and fs.nr_open to 2^30-8 (otherwise a 2 GB guest keeps the
# kernel's default of ~200k open files system-wide, and a non-root server's accepts fail with
# ENFILE near 200k connections, which the official box does not do); systemd's 50-default.conf
# and Ubuntu's 10-bufferbloat.conf select fq_codel; Ubuntu's 10-network-security.conf keeps SYN
# cookies on. Nothing else is changed from kernel defaults.
echo 9223372036854775807 > /proc/sys/fs/file-max
echo 1073741816 > /proc/sys/fs/nr_open
echo fq_codel > /proc/sys/net/core/default_qdisc
echo 1 > /proc/sys/net/ipv4/tcp_syncookies
echo "@BOOT $(uname -r) variant=$V nproc=$(nproc) $(grep MemTotal /proc/meminfo)"
echo "@SYSCTL file-max=$(< /proc/sys/fs/file-max) tcp_mem=$(< /proc/sys/net/ipv4/tcp_mem) somaxconn=$(< /proc/sys/net/core/somaxconn) syn_backlog=$(< /proc/sys/net/ipv4/tcp_max_syn_backlog) qdisc=$(< /proc/sys/net/core/default_qdisc)"
dmesg -w > /dev/hvc2 2>&1 &
ip link set lo up; ip addr add 10.153.0.2/16 dev eth0; ip link set eth0 up
[ -x /variants/$V/server ] || { echo "@FAIL no variant $V"; sleep 5; echo o > /proc/sysrq-trigger; }
cd /work
# The app runs in its own cgroup, as systemd's service unit does on the scoring box; the leaderboard's
# CPU and RAM columns are that cgroup's cpu.stat usage and memory.peak.
echo "+memory +cpu" > /sys/fs/cgroup/cgroup.subtree_control
mkdir -p /sys/fs/cgroup/app
CG=/sys/fs/cgroup/app
sh -c 'echo $$ > /sys/fs/cgroup/app/cgroup.procs; exec "$@"' sh prlimit --nofile=65535:65535 setpriv --reuid=65534 --regid=65534 --clear-groups \
  env SQLITE_PATH=/work/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 \
  /variants/$V/server > /dev/hvc1 2>&1 &
P=$!
ok=0
health() (   # subshell: a failed /dev/tcp redirection must not touch PID 1
  exec 3<> /dev/tcp/127.0.0.1/3000 || exit 1
  printf 'GET /health HTTP/1.1\r\nHost: vm\r\n\r\n' >&3
  read -t 2 -r line <&3 && [[ $line == *" 200"* ]]
)
for i in $(seq 600); do
  health 2> /dev/null && { ok=1; break; }
  kill -0 $P 2> /dev/null || break
  sleep 0.1
done
[ $ok = 1 ] && echo "@READY $V $P" || echo "@FAIL server did not come up"
# Optional profile (perf_at/perf_for/perf_report_at, seconds of guest uptime): system-wide, 499 Hz,
# call graphs; the report is produced after the hold (perf_report_at) so it does not cost hold CPU.
if [ -n "$PA" ] && [ -n "$PF" ] && command -v perf > /dev/null; then
  ( while read -r up _ < /proc/uptime; [ "${up%.*}" -lt "$PA" ]; do sleep 0.5; done
    perf record -q -a -g -F 499 -o /tmp/perf.data -- sleep "$PF" > /dev/null 2>&1
    while read -r up _ < /proc/uptime; [ "${up%.*}" -lt "${PR:-0}" ]; do sleep 0.5; done
    { echo "== comm,dso"; perf report -i /tmp/perf.data --no-children --sort comm,dso --stdio -g none --percent-limit 0.3 2>/dev/null
      echo "== sym"; perf report -i /tmp/perf.data --no-children --sort dso,sym --stdio -g none --percent-limit 0.25 2>/dev/null
      echo "== end"; } > /dev/hvc3 2>&1 ) &
fi
mkfifo /tmp/tick; exec 9<> /tmp/tick
# Network counters every 5 s (headers once): TcpExt (/proc/net/netstat), Tcp (/proc/net/snmp), the
# CPU's softnet_stat line (hex: processed, dropped, time_squeeze, ...) and ksoftirqd/0's CPU time.
echo "@NXH $(grep -m1 '^TcpExt:' /proc/net/netstat)"
echo "@NTH $(grep -m1 '^Tcp:' /proc/net/snmp)"
KS=$(pgrep -x ksoftirqd/0)
k=0
while :; do
  if (( k++ % 5 == 0 )); then
    nx=; nt=; f1=1; f2=1
    while read -r a rest; do [ "$a" = TcpExt: ] && { [ $f1 = 1 ] && f1=0 || nx=$rest; }; done < /proc/net/netstat
    while read -r a rest; do [ "$a" = Tcp: ] && { [ $f2 = 1 ] && f2=0 || nt=$rest; }; done < /proc/net/snmp
    read -r sn < /proc/net/softnet_stat; ks=; [ -n "$KS" ] && read -r ks < /proc/$KS/stat
    read -r up _ < /proc/uptime
    echo "@N $up | $nx | $nt | $sn | ${ks##*) }"
  fi
  read -r up _ < /proc/uptime
  read -r c < /proc/stat
  s=; m=; r=; t=
  [ -r /proc/$P/stat ] && read -r s < /proc/$P/stat
  while read -r k v _; do [ "$k" = MemAvailable: ] && { m=$v; break; }; done < /proc/meminfo
  [ -r /proc/$P/status ] && while read -r k v _; do [ "$k" = VmRSS: ] && { r=$v; break; }; done < /proc/$P/status
  while read -r l; do case $l in TCP:*) t=$l ;; esac; done < /proc/net/sockstat
  cgc=; cgp=; cgu=; ms=
  read -r cgc < $CG/memory.current; read -r cgp < $CG/memory.peak
  while read -r k v; do [ "$k" = usage_usec ] && { cgu=$v; break; }; done < $CG/cpu.stat
  while read -r k v; do case $k in anon|file|kernel|sock|slab) ms="$ms $k=$v" ;; esac; done < $CG/memory.stat
  echo "@S $up | $c | ${s##*) } | $m $r | $t | $cgc $cgp $cgu$ms"
  read -t 1 -u 9 _
done
