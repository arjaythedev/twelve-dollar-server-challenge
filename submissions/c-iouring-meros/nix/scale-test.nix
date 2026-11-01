# Two virtual machines on a virtual network: the server as the challenge describes its box, and a client that
# generates the load.
#
#   server  1 vCPU, 2 GB RAM, no swap, systemd service with LimitNOFILE=65535 and CAP_NET_BIND_SERVICE,
#           an unprivileged user, port 80, a fresh copy of the seed database
#   client  4 vCPUs, 6 GB RAM, several source addresses (one address gives at most ~64k connections)
#
# The pass criteria are the challenge's: p95 < 500 ms, p99 < 1 s, < 1% failed requests.
{ pkgs, server, seed, loadgen, challenge }:

let
  # The test framework boots with clocksource=acpi_pm, an emulated timer whose every read is a VM exit; the kernel reads
  # the clock for every packet, so that setting makes the network stack several times more expensive than on a real
  # droplet, which uses kvm-clock. Boot with kvm-clock, as a droplet does.
  realisticClock = { lib, ... }: { boot.kernelParams = lib.mkAfter [ "clocksource=kvm-clock" ]; };
in

{ users
, rampSeconds ? 60
, holdSeconds ? 60
, fdLimit ? 65535       # the server's LimitNOFILE
, useK6 ? false         # real k6 instead of the C load generator (k6 needs ~1 MB per user: keep `users` small)
, serverMemoryMB ? 2048
, serverPackage ? server  # a build variant to compare
, profile ? false       # perf-profile the whole server VM during the hold phase
, clientCores ? 4
, salt ? ""            # changes the derivation, so an identical test runs again instead of coming from the cache
, think ? "1.0"           # scales every think time of the users: 2.0 halves the request rate, 0.66 raises it by half

}:

pkgs.testers.runNixOSTest {
  name = "scale${salt}-${toString users}-think${think}-fd${toString fdLimit}${if useK6 then "-k6" else ""}-${serverPackage.name}${if profile then "-profile" else ""}";

  nodes.server = { ... }: {
    imports = [ realisticClock ];
    virtualisation = { cores = 1; memorySize = serverMemoryMB; diskSize = 3072; };
    environment.systemPackages = [ pkgs.perf pkgs.iproute2 ];
    networking.firewall.enable = false;
    systemd.services.feed = {
      wantedBy = [ "multi-user.target" ];
      after = [ "network.target" ];
      environment = {
        SQLITE_PATH = "/var/lib/feed/feed.db";
        JWT_SECRET = "twelve-dollar-challenge";
        HOST = "0.0.0.0";
        PORT = "80";
      };
      serviceConfig = {
        ExecStartPre = "${pkgs.coreutils}/bin/cp --no-preserve=mode ${seed}/feed.db /var/lib/feed/feed.db";
        ExecStart = "${serverPackage}/bin/server";
        DynamicUser = true;
        StateDirectory = "feed";
        LimitNOFILE = fdLimit;
        AmbientCapabilities = "CAP_NET_BIND_SERVICE";
        TimeoutStartSec = 120;
      };
    };
  };

  nodes.client = { ... }: {
    imports = [ realisticClock ];
    virtualisation = { cores = clientCores; memorySize = 6144; };
    networking.firewall.enable = false;
    boot.kernel.sysctl."net.ipv4.ip_local_port_range" = "1024 65535";
    environment.systemPackages = [ loadgen pkgs.k6 pkgs.jq pkgs.procps pkgs.iproute2 ];
  };

  testScript = ''
    import re

    start_all()
    try:
        server.wait_for_unit("feed.service")
    except Exception:
        print("SERVER feed.service failed:")
        print(server.execute("journalctl -u feed --no-pager | tail -25")[1])
        raise
    server.wait_for_open_port(80)
    client.wait_for_unit("multi-user.target")
    client.succeed("curl -sf http://server/health")

    # More source addresses: each one carries ~64k connections to the one server port.
    sources = ",".join(f"192.168.1.{100 + i}" for i in range(1, ${toString (users / 50000 + 2)} + 1))
    for address in sources.split(","):
        client.succeed(f"ip addr add {address}/24 dev eth1")

    def cpu_times():
        # /proc/stat on the one vCPU: user nice system idle iowait irq softirq steal. "steal" is time the host
        # gave to someone else, so it is not our cost; the network stack shows up as system + irq + softirq.
        f = [int(x) for x in server.succeed("head -1 /proc/stat").split()[1:9]]
        return dict(user=f[0] + f[1], system=f[2], idle=f[3] + f[4], irq=f[5], softirq=f[6], steal=f[7])

    def usage(a, b):
        delta = {k: b[k] - a[k] for k in a}
        total = sum(delta.values())
        own = total - delta["steal"]
        busy = own - delta["idle"]
        return busy, own, delta

    print("server clocksource:", server.succeed("cat /sys/devices/system/clocksource/clocksource0/current_clocksource").strip())
    print("server CPU:", server.succeed("grep -m1 'model name' /proc/cpuinfo").strip())
    server.succeed("""
      ( while sleep 5; do
          echo "$(date +%s) $(grep -E 'MemAvailable|^Slab|SUnreclaim' /proc/meminfo | tr -s ' ' | tr '\\n' ' ') open=$(ls /proc/$(systemctl show -p MainPID --value feed)/fd | wc -l) acceptq=$(ss -ltnH 'sport = :80' | awk '{print $2}') tcpmem=$(grep '^TCP:' /proc/net/sockstat | sed -E 's/.* mem ([0-9]+).*/\\1/') tcpalloc=$(grep '^TCP:' /proc/net/sockstat | sed -E 's/.* alloc ([0-9]+).*/\\1/')"
        done > /tmp/memory.log 2>&1 & )
    """)

    ${if useK6 then ''
      client.succeed(
          "systemd-run --unit=lg --no-block -p LimitNOFILE=1048576 -p StandardOutput=file:/tmp/lg.out -p StandardError=file:/tmp/lg.out "
          "k6 run --quiet --no-color -e BASE_URL=http://server -e VUS=${toString users} "
          "-e RAMP_UP=${toString rampSeconds}s -e DURATION=${toString holdSeconds}s "
          "-e TOKENS=${seed}/tokens.json ${challenge}/bench/load.js"
      )
    '' else ''
      client.succeed(
          f"systemd-run --unit=lg --no-block -p LimitNOFILE=1048576 -p StandardOutput=file:/tmp/lg.out -p StandardError=file:/tmp/lg.out "
          f"-E SRC_IPS={sources} -E THINK=${think} ${loadgen}/bin/loadgen server 80 ${toString users} ${toString rampSeconds} ${toString holdSeconds} ${toString clientCores} ${seed}/tokens.json"
      )
    ''}
    def machine_cpu(machine):
        f = [int(x) for x in machine.succeed("head -1 /proc/stat").split()[1:9]]
        return dict(user=f[0] + f[1], system=f[2], idle=f[3] + f[4], irq=f[5], softirq=f[6], steal=f[7])

    counters = "TcpExtListenOverflows TcpExtListenDrops TcpExtTCPReqQFullDrop TcpExtTCPReqQFullDoCookies TcpExtTCPTimeouts TcpExtTCPSynRetrans TcpRetransSegs TcpExtTCPAbortOnData TcpExtTCPAbortOnClose TcpExtTW TcpExtTCPTimeWaitOverflow"
    server.succeed("nstat -n")      # reset the baseline of the incremental counters
    client.succeed("nstat -n")
    t_start = cpu_times()
    c_start = machine_cpu(client)
    server.sleep(${toString rampSeconds} + 3)          # the ramp is over
    t_hold_start = cpu_times()
    c_hold_start = machine_cpu(client)
    ${if profile then ''
      server.succeed("perf record -F 999 -g -a -o /tmp/perf.data -- sleep 12 2>&1 | tail -2")
      server.sleep(${toString holdSeconds} - 8 - 14)
    '' else ''
      server.sleep(${toString holdSeconds} - 8)
    ''}
    t_hold_end = cpu_times()
    c_hold_end = machine_cpu(client)
    client.wait_until_succeeds("! systemctl is-active lg", timeout=600)
    out = client.succeed("cat /tmp/lg.out")
    print(out[-3000:])

    for label, (x, y) in {"ramp": (t_start, t_hold_start), "hold": (t_hold_start, t_hold_end)}.items():
        busy, own, delta = usage(x, y)
        parts = ", ".join(f"{k} {100.0 * v / own:.1f}%" for k, v in delta.items() if k != "steal" and k != "idle")
        print(f"SERVER vCPU {label}: busy {100.0 * busy / own:.1f}% ({parts}); steal {100.0 * delta['steal'] / (own + delta['steal']):.1f}% of wall time")

    ${if profile then ''
      print("PERF by symbol (whole vCPU, hold phase):")
      print(server.succeed("perf report -i /tmp/perf.data --no-children --sort sym --stdio -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -45"))
      print("PERF by command:")
      print(server.succeed("perf report -i /tmp/perf.data --no-children --sort comm --stdio -g none 2>/dev/null | grep -v '^#' | grep -v '^$' | head -8"))
    '' else ""}
    busy, own, delta = usage(c_hold_start, c_hold_end)
    print(f"CLIENT vCPU hold: busy {100.0 * busy / own:.1f}% over ${toString clientCores} vCPUs (user {100.0 * delta['user'] / own:.1f}%, system {100.0 * delta['system'] / own:.1f}%, softirq {100.0 * delta['softirq'] / own:.1f}%)")
    print("SERVER kernel counters since the start:")
    print(server.succeed("nstat -az " + counters + " | tr -s ' '"))
    print("CLIENT kernel counters since the start:")
    print(client.succeed("nstat -az TcpExtTCPTimeouts TcpExtTCPSynRetrans TcpRetransSegs TcpExtTCPAbortOnData | tr -s ' '"))
    print("SERVER interface counters:", server.succeed("ip -s link show eth1 | tr -s ' ' | tr '\\n' ' '"))
    print("CLIENT interface counters:", client.succeed("ip -s link show eth1 | tr -s ' ' | tr '\\n' ' '"))
    print("CLIENT free memory (MB):", client.succeed("grep MemAvailable /proc/meminfo").split()[1])
    memory = server.succeed("cat /tmp/memory.log")
    available = [int(m.group(1)) for m in re.finditer(r"MemAvailable: (\d+)", memory)]
    slab = [int(m.group(1)) for m in re.finditer(r"SUnreclaim: (\d+)", memory)]
    opened = [int(m.group(1)) for m in re.finditer(r"open=(\d+)", memory)]
    acceptq = [int(m.group(1)) for m in re.finditer(r"acceptq=(\d+)", memory)]
    tcpmem = [int(m.group(1)) for m in re.finditer(r"tcpmem=(\d+)", memory)]
    tcpalloc = [int(m.group(1)) for m in re.finditer(r"tcpalloc=(\d+)", memory)]
    print("SERVER tcp_mem limits (pages: low, pressure, hard):", server.succeed("cat /proc/sys/net/ipv4/tcp_mem").strip())
    print("SERVER time series (every 5 s): seconds since start, MemAvailable MB, slab MB, open fds, accept queue, TCP mem pages, TCP sockets")
    first = None
    for line in memory.splitlines():
        m = re.match(r"(\d+) .*MemAvailable: (\d+).*SUnreclaim: (\d+).*open=(\d+) acceptq=(\d*) tcpmem=(\d+) tcpalloc=(\d+)", line)
        if m:
            first = first or int(m.group(1))
            print(f"  t={int(m.group(1)) - first:4d}s avail={int(m.group(2)) // 1024:5d}MB slab={int(m.group(3)) // 1024:4d}MB tcp_mem={m.group(6):>7} sockets={m.group(7):>7} acceptq={m.group(5) or 0}")
    print(f"SERVER TCP memory in pages, max over 5 s samples: {max(tcpmem) if tcpmem else 'n/a'}; sockets allocated max: {max(tcpalloc) if tcpalloc else 'n/a'}")
    print(f"SERVER accept queue (Recv-Q of the listener), max over 5 s samples: {max(acceptq) if acceptq else 'n/a'} of 4096")
    print(f"SERVER min MemAvailable: {min(available) // 1024} MB, max unreclaimable slab: {max(slab) // 1024} MB, max open descriptors: {max(opened)}")
    server.succeed("systemctl is-active feed")   # still alive: no crash, no OOM kill
    print("SERVER slow rounds logged:", server.succeed("journalctl -u feed --no-pager | grep -c 'slow round' || true").strip())
    print(server.succeed("journalctl -u feed --no-pager | grep 'slow round' | head -6 || true"))
    print("SERVER memwatch lines:")
    print(server.succeed("journalctl -u feed --no-pager | grep memwatch | head -24 || true"))
    print(server.succeed("dmesg | grep -i -E 'out of memory|oom|time wait|syn flood|possible SYN' | tail -5 || true"))

    ${if useK6 then ''
      # k6 exits non-zero when a threshold (p95 < 500 ms, p99 < 1 s, < 1% failed requests) is crossed.
      status = client.succeed("systemctl show lg -p ExecMainStatus --value").strip()
      assert status == "0", "k6 thresholds crossed:\n" + out[-2500:]
      print("RESULT k6: all thresholds passed with users=${toString users} on fdLimit=${toString fdLimit}")
    '' else ''
      m = re.search(r"^HOLD\s+n=(\d+) fail=(\d+) \(([\d.]+)%\) p50=([\d.]+)ms p95=([\d.]+)ms p99=([\d.]+)ms", out, re.M)
      assert m, "no HOLD line in the load generator output"
      failed, p95, p99 = float(m.group(3)), float(m.group(5)), float(m.group(6))
      print(f"RESULT users=${toString users} fdLimit=${toString fdLimit}: failed={failed}% p95={p95}ms p99={p99}ms")
      assert failed < 1.0, f"{failed}% failed requests"
      assert p95 < 500 and p99 < 1000, f"latency p95={p95} p99={p99}"
    ''}
  '';
}
