"""Verify local cgroup reclaim and OOM with a disposable allocation process, outside scored runs."""

import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_memory import HERE, Manager, base, evaluate
from monitor import APP, CG, GUEST, budget, private_paths, require
from prove import CPU, as_user, setup_route, wait_for


def line(p):
    if not select.select([p.stdout], [], [], 10)[0]:
        raise TimeoutError("private helper response")
    return p.stdout.readline().strip()


def run(a):
    require(os.getuid() != 0 and os.sched_getaffinity(0) == {CPU}, "lease only CPU 9")
    marker = "tr" + uuid.uuid4().hex[:7]
    sl, unit = private_paths(marker)
    names = []
    require(not (CG / sl).exists(), "fresh empty slice")
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    app = APP // 64
    guest = GUEST // 64
    holder = manager = peers = None
    started = False
    with tempfile.TemporaryDirectory(prefix="private-pressure-", dir=a.work) as temp:
        try:
            nap = setup_route(marker, names)
            props = [
                f"Slice={sl}",
                f"User={os.getuid()}",
                f"Group={os.getgid()}",
                "NoNewPrivileges=yes",
                "LimitNOFILE=4096",
                "MemorySwapMax=0",
                f"MemoryMax={app}",
                f"CPUAffinity={CPU}",
                f"AllowedCPUs={CPU}",
                f"NetworkNamespacePath=/run/netns/{marker}s",
                "CapabilityBoundingSet=",
                "AmbientCapabilities=",
            ]
            with (out / "holder.log").open("w") as log:
                holder = subprocess.Popen(
                    [
                        "sudo",
                        "-n",
                        "systemd-run",
                        "--quiet",
                        "--collect",
                        "--pipe",
                        "--wait",
                        "--unit=" + unit,
                        *["--property=" + v for v in props],
                        str(a.holder),
                        str(Path(temp) / "cache"),
                    ],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=log,
                    text=True,
                )
                started = True
                reply = line(holder).split()
                require(reply and reply[0] == "READY", "holder startup")
                pid = int(reply[1])
                manager = Manager(out, marker, pid, a.holder, CPU, CPU, 19494, app, guest, 100)

                def last(check):
                    def observe():
                        for raw in reversed((out / "passive-memory.jsonl").read_text().splitlines()):
                            try:
                                r = json.loads(raw)
                            except ValueError:
                                continue
                            if r["kind"] == "sample" and check(r):
                                return r

                    return wait_for(observe, "memory accounting did not settle", 12)

                start = time.time()
                holder.stdin.write("cache\n")
                holder.stdin.flush()
                require(line(holder) == "CACHED", "private file cache did not become clean")
                cached = last(lambda r: r["reclaim"]["file"] > app * 0.75)
                peers = subprocess.Popen(
                    as_user(marker + "c0", "/usr/bin/python3", HERE / "idle_peers.py"),
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=log,
                    text=True,
                )
                require(line(peers) == "READY 2304", "external cohort did not complete its handshakes")
                high = last(lambda r: r["count"] == 2304 and r["cap"] == budget(app, guest, 2304))
                holder.stdin.write("anon\n")
                holder.stdin.flush()
                require(line(holder) == "ANON", "local allocation did not start")
                require(manager.proc.wait(timeout=15) == 0, "monitor did not observe private holder exit")
                manager.close()
                manager = None
                require(holder.wait(timeout=4) != 0, "disposable holder survived its local memory limit")
                time.sleep(0.02)
                cut = time.time()
                base.write(out / "timing.json", dict(stock_start_unix=start, cutoff_unix=cut))
                result = evaluate(out)
                rows = [json.loads(x) for x in (out / "passive-memory.jsonl").read_text().splitlines()]
                end = rows[-1]
                require(end["reason"] == "service_exit", "missing pidfd lifetime event")
                require(
                    result["oom_kill_at_cutoff"] >= 1
                    and end.get("parent_events_local", {}).get("oom", 0) >= 1,
                    "missing parent-local OOM attribution: " + str(end),
                )
                require(
                    high["reclaim"]["pgsteal"] >= cached["reclaim"]["pgsteal"]
                    and high["current"] <= high["cap"] + 4096 * 64,
                    "uncapped private cache",
                )
                require(
                    peers.poll() is None and os.sched_getaffinity(0) == {CPU},
                    "external generator was affected by service OOM",
                )
                peers.stdin.close()
                require(peers.wait(timeout=4) == 0, "external peer cleanup failed")
                proof = dict(
                    clean_cache=cached,
                    at_private_tcp_cap=high,
                    service_exit=end,
                    caller_and_external_peers_survived=True,
                    manager=result,
                    private_napi=nap,
                )
                base.write(out / "passive-memory-result.json", result)
        finally:
            if peers and peers.poll() is None:
                peers.stdin.close()
                peers.wait(timeout=6)
            if started:
                base.priv("systemctl", "stop", unit, check=False)
            if manager:
                manager.close()
            if holder and holder.poll() is None:
                holder.wait(timeout=6)
            if (CG / sl).exists():
                base.priv("systemctl", "stop", sl)
            for ns in reversed(names):
                base.priv("ip", "netns", "del", ns)
        proof["cleanup"] = not (CG / sl).exists() and not any(
            (Path("/run/netns") / n).exists() for n in names
        )
        require(proof["cleanup"], "private fixture cleanup")
        base.write(out / "pressure-proof.json", proof)
        print(
            json.dumps(
                {
                    k: v
                    for k, v in proof.items()
                    if k in ("service_exit", "cleanup", "caller_and_external_peers_survived")
                }
            ),
            flush=True,
        )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--holder", type=Path, required=True)
    run(p.parse_args())
