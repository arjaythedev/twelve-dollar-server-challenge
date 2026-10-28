#!/usr/bin/env python3
"""Run local Linux k6 -> WSL server tests with verified cgroup limits.

Requires a published Linux server, an existing seed, Linux k6 2.3.0, and WSL
Ubuntu with systemd. Results are local diagnostics, not a remote-VM score.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request
import uuid


def utc():
    return datetime.now(timezone.utc).isoformat()


def duration_seconds(text):
    return float(text[:-1]) * {"s": 1, "m": 60}[text[-1]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vus", nargs="+", type=int, default=[2500, 5000, 10000])
    parser.add_argument("--hold", default="60s")
    parser.add_argument("--ramp", default="15s")
    parser.add_argument("--warmup", default="0s")
    parser.add_argument("--distribution", default="Ubuntu")
    parser.add_argument("--port", type=int, default=3108)
    parser.add_argument("--k6", type=Path)
    parser.add_argument("--publish", type=Path, help="Linux publish directory on the Windows filesystem; defaults to bin/publish")
    parser.add_argument("--collect-counters", action="store_true", help="Collect runtime and database metrics using pinned Linux dotnet-counters")
    parser.add_argument("--client-ips", help="Comma-separated IPv4 loopback source addresses for k6, e.g. 127.0.0.1,127.0.0.2")
    parser.add_argument("--seed", help="Linux path to the seed; defaults to the current WSL user's native cache")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Run this orchestrator from Windows; both workloads execute in WSL.")
    if any(vus < 1 for vus in args.vus):
        parser.error("VUS must be positive")
    try:
        client_ips = [ipaddress.ip_address(value.strip()) for value in args.client_ips.split(",")] if args.client_ips else []
    except ValueError as error:
        parser.error(str(error))
    if any(address.version != 4 or not address.is_loopback for address in client_ips):
        parser.error("Client source addresses must be IPv4 loopback addresses")
    if len(set(client_ips)) != len(client_ips):
        parser.error("Client source addresses must be distinct")
    client_network_options = ["--local-ips", ",".join(map(str, client_ips))] if client_ips else []
    duration_seconds(args.hold)
    duration_seconds(args.ramp)
    duration_seconds(args.warmup)
    submission = Path(__file__).resolve().parents[1]
    repository = submission.parents[1]
    publish = (args.publish or submission / "bin/publish").resolve()
    k6 = args.k6 or submission / ".tools/k6/k6-v2.3.0-linux-amd64/k6"
    if not k6.is_file() or not (publish / "FeedApi").is_file():
        parser.error("Install Linux k6 and run build.sh first (see README).")
    creationflags = subprocess.CREATE_NO_WINDOW

    def wsl(*command, root=False, check=True):
        prefix = ["wsl.exe", "-d", args.distribution]
        if root:
            prefix += ["-u", "root"]
        result = subprocess.run(prefix + ["--exec", *map(str, command)], capture_output=True,
                                text=True, encoding="utf-8", check=False, creationflags=creationflags)
        if check and result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip())
        return result.stdout.strip()

    linux_submission = wsl("wslpath", "-u", str(submission))
    linux_user = wsl("id", "-un")
    linux_home = wsl("bash", "-lc", 'printf "%s" "$HOME"')
    linux_publish = wsl("wslpath", "-u", str(publish))
    ephemeral_ports = list(map(int, wsl("cat", "/proc/sys/net/ipv4/ip_local_port_range").split()))
    available_client_ports = (ephemeral_ports[1] - ephemeral_ports[0] + 1) * max(1, len(client_ips))
    if max(args.vus) > available_client_ports:
        parser.error(f"Requested VUs exceed the {available_client_ports} possible client source ports; "
                     "use --client-ips 127.0.0.1,127.0.0.2 or a separate load generator")
    counter_tool = linux_home + "/.cache/twelve-dollar-server-challenge/tools/diagnostics/dotnet-counters"
    dotnet_root = "/opt/twelve-dollar-dotnet/10.0.401"
    counter_version = wsl("env", "DOTNET_ROOT=" + dotnet_root, counter_tool, "--version") if args.collect_counters else None
    seed = args.seed or linux_home + "/.cache/twelve-dollar-server-challenge/seed/feed.db"
    linux_k6 = linux_home + "/.cache/twelve-dollar-server-challenge/tools/k6-2.3.0"
    wsl("mkdir", "-p", linux_home + "/.cache/twelve-dollar-server-challenge/tools")
    wsl("cp", wsl("wslpath", "-u", str(k6)), linux_k6)
    wsl("chmod", "+x", linux_k6)
    group_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    results = repository / "results" / ("performance-" + group_id)
    results.mkdir(parents=True)
    source_files = [path for path in sorted(submission.rglob("*.cs"))
                    if not {"bin", "obj"}.intersection(path.relative_to(submission).parts)]
    application_hashes = {path.relative_to(submission).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [*source_files, submission / "FeedApi.csproj", submission / "packages.lock.json"]}
    all_results = []

    def run_k6(label, vus, hold, ramp, base_url, output, pid_file):
        summary_path = output / f"{label}-summary.json"
        environment = dict(os.environ, K6_NO_USAGE_REPORT="true", NO_PROXY="*", no_proxy="*")
        command = ["wsl.exe", "-d", args.distribution, "--exec", "bash", linux_submission + "/checks/run-k6-local.sh",
                   pid_file, "env", "K6_NO_USAGE_REPORT=true", "NO_PROXY=*", "no_proxy=*", linux_k6,
                   "run", "--quiet", "--no-color", *client_network_options, "--env", f"VUS={vus}",
                   "--env", f"DURATION={hold}", "--env", f"RAMP_UP={ramp}", "--env", f"BASE_URL={base_url}",
                   "--env", f"TOKENS={wsl('wslpath', '-u', str(repository / 'seed/tokens.json'))}",
                   "--env", f"RESULT_JSON={wsl('wslpath', '-u', str(summary_path))}", linux_submission + "/checks/load-local.js"]
        started = utc()
        with (output / f"{label}-stdout.log").open("w") as stdout, \
             (output / f"{label}-stderr.log").open("w") as stderr:
            process = subprocess.Popen(command, cwd=repository, env=environment, stdout=stdout, stderr=stderr,
                                       stdin=subprocess.DEVNULL, creationflags=creationflags)
            last_progress = time.monotonic()
            deadline = last_progress + duration_seconds(hold) + duration_seconds(ramp) + 120
            try:
                while process.poll() is None:
                    now = time.monotonic()
                    if now - last_progress >= 30:
                        print(f"  {label}: {vus} VUs running", flush=True)
                        last_progress = now
                    if now > deadline:
                        process.kill()
                        raise TimeoutError("k6 exceeded its expected run duration")
                    time.sleep(2)
                process.wait()
            finally:
                if process.poll() is None:
                    owned_pid = int(wsl("cat", pid_file, check=False) or "0")
                    if owned_pid and wsl("readlink", "-f", f"/proc/{owned_pid}/exe", check=False) == linux_k6:
                        wsl("kill", "-TERM", str(owned_pid), check=False)
                    process.kill()
                    process.wait()
        if not summary_path.is_file():
            raise RuntimeError(f"k6 produced no summary: {(output / (label + '-stderr.log')).read_text()}")
        summary = json.loads(summary_path.read_text())
        return summary, {"exit_code": process.returncode, "started_utc": started, "finished_utc": utc()}

    for vus in args.vus:
        output = results / str(vus)
        output.mkdir()
        unit = f"tdsc-local-{group_id.lower()}-{vus}"
        native_dir = f"{linux_home}/.cache/twelve-dollar-server-challenge/performance/{group_id}/{vus}"
        database = native_dir + "/feed.db"
        monitor = None
        collector = None
        collector_log = (output / "counters.log").open("w") if args.collect_counters else None
        monitor_log = (output / "monitor.log").open("w")
        metadata = {"vus": vus, "hold": args.hold, "ramp": args.ramp, "warmup": args.warmup,
                    "created_utc": utc(), "host_logical_cpus": os.cpu_count(), "unit": unit, "database": database,
                    "k6_version": wsl(linux_k6, "version"),
                    "application_source_sha256": application_hashes,
                    "publish_directory": str(publish),
                    "publish_application_sha256": hashlib.sha256((publish / "FeedApi.dll").read_bytes()).hexdigest(),
                    "counter_tool_version": counter_version,
                    "counter_timestamp_timezone": "UTC" if args.collect_counters else None,
                    "client_local_ips": list(map(str, client_ips)) or None,
                    "client_ephemeral_port_range": ephemeral_ports,
                    "workload_sha256": hashlib.sha256((repository / "bench/load.js").read_bytes()).hexdigest(),
                    "limitations": "Linux k6 and server share WSL hardware. CPU quota differs from a 1-vCPU VM. OS outside server cgroup is not included."}
        try:
            wsl("mkdir", "-p", native_dir)
            wsl("systemd-run", "--quiet", "--unit=" + unit, "--property=User=" + linux_user,
                "--property=CPUQuota=100%", "--property=MemoryMax=2G", "--property=MemorySwapMax=0",
                "--property=LimitNOFILE=65535", "--setenv=SQLITE_PATH=" + database,
                "--setenv=JWT_SECRET=twelve-dollar-challenge", "--setenv=HOST=0.0.0.0",
                "--setenv=PORT=" + str(args.port), "bash", linux_submission + "/checks/benchmark-server.sh", seed, linux_publish, root=True)
            cgroup = wsl("systemctl", "show", unit, "--property=ControlGroup", "--value")
            pid = int(wsl("systemctl", "show", unit, "--property=MainPID", "--value"))
            metadata["server_pid"] = pid
            process_limits = wsl("cat", f"/proc/{pid}/limits")
            nofile = next(line.split()[3:5] for line in process_limits.splitlines() if line.startswith("Max open files"))
            assert nofile == ["65535", "65535"], nofile
            metadata["nofile_limits"] = nofile
            limits = {name: wsl("cat", "/sys/fs/cgroup" + cgroup + "/" + name) for name in
                      ("cpu.max", "memory.max", "memory.swap.max")}
            assert limits == {"cpu.max": "100000 100000", "memory.max": "2147483648", "memory.swap.max": "0"}, limits
            metadata["limits"] = limits
            linux_ip = wsl("hostname", "-I").split()[0]
            base_url = f"http://127.0.0.1:{args.port}"
            metadata["base_url"] = base_url
            client_pid_file = native_dir + "/k6.pid"
            monitor = subprocess.Popen(["wsl.exe", "-d", args.distribution, "--exec", "python3",
                linux_submission + "/checks/monitor-server.py", "--cgroup", cgroup, "--pid", str(pid),
                "--database", database, "--output", wsl("wslpath", "-u", str(output / "server.csv")),
                "--client-pid-file", client_pid_file],
                stdout=monitor_log, stderr=subprocess.STDOUT, creationflags=creationflags)
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            for _ in range(60):
                try:
                    with opener.open(f"http://{linux_ip}:{args.port}/health", timeout=2) as response:
                        if response.status == 200:
                            break
                except OSError:
                    time.sleep(1)
            else:
                raise RuntimeError("server did not become healthy")
            if args.collect_counters:
                collector = subprocess.Popen(["wsl.exe", "-d", args.distribution, "--exec", "env",
                    "DOTNET_ROOT=" + dotnet_root, "TZ=UTC", counter_tool, "collect", "--process-id", str(pid),
                    "--counters", "System.Runtime,DevvGuru.FeedApi", "--refresh-interval", "2",
                    "--format", "csv", "--output", wsl("wslpath", "-u", str(output / "counters.csv"))],
                    stdout=collector_log, stderr=subprocess.STDOUT, creationflags=creationflags)
                # Attach before traffic; diagnostics overhead is present in both comparison runs.
                for _ in range(20):
                    if collector.poll() is not None:
                        raise RuntimeError("dotnet-counters exited before load; see counters.log")
                    # This tool exports its CSV only when the session stops.
                    if "Starting a counter session" in (output / "counters.log").read_text():
                        break
                    time.sleep(0.5)
                else:
                    raise RuntimeError("dotnet-counters did not begin collecting")
            print(f"RUN {vus} VUs, {args.hold} hold; verified 1 CPU / 2 GiB / no swap; {base_url}", flush=True)
            if duration_seconds(args.warmup) > 0:
                _, metadata["warmup_run"] = run_k6("warmup", 1000, args.warmup, "15s", base_url, output, client_pid_file)
            summary, metadata["measured_run"] = run_k6("measured", vus, args.hold, args.ramp, base_url, output, client_pid_file)
            metrics = summary["metrics"]
            duration = metrics["http_req_duration"]["values"]
            result = {"vus": vus, "hold": args.hold, "ramp": args.ramp,
                      "pass": metadata["measured_run"]["exit_code"] == 0,
                      "overall_rps": metrics["http_reqs"]["values"]["rate"],
                      "requests": metrics["http_reqs"]["values"]["count"],
                      "p95_ms": duration["p(95)"], "p99_ms": duration["p(99)"], "max_ms": duration["max"],
                      "error_rate": metrics["http_req_failed"]["values"]["rate"], "directory": str(output)}
            all_results.append(result)
            print("RESULT " + json.dumps(result), flush=True)
            (results / "summary.json").write_text(json.dumps(all_results, indent=2))
        finally:
            metadata["service_final"] = wsl("systemctl", "show", unit, "--property=ActiveState,Result,ExecMainStatus,MemoryPeak,CPUUsageNSec", check=False)
            (output / "server.log").write_text(wsl("journalctl", "-u", unit, "--no-pager", "-o", "cat", root=True, check=False))
            wsl("systemctl", "stop", unit, root=True, check=False)
            wsl("systemctl", "reset-failed", unit, root=True, check=False)
            if collector:
                try:
                    collector.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    collector.terminate()
                    collector.wait(timeout=5)
                metadata["counter_collector_exit_code"] = collector.returncode
            if collector_log:
                collector_log.close()
            if monitor:
                try:
                    monitor.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    monitor.terminate()
                    monitor.wait(timeout=5)
            monitor_log.close()
            (output / "metadata.json").write_text(json.dumps(metadata, indent=2))
    print("RESULTS " + str(results), flush=True)


if __name__ == "__main__":
    main()
