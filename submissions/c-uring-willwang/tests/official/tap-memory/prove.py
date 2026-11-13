"""Scaled real-service check for the optional memory fixture. Run as an ordinary user on CPU 9."""

import argparse
import json
import os
from pathlib import Path
import select
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_memory import HERE, Manager, base, evaluate
from monitor import APP, CG, GUEST, MIB, PER_SOCKET, budget, private_paths, require

CPU = 9
PORT = 19493


def wait_for(test, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = test()
        if found:
            return found
        time.sleep(0.025)
    raise TimeoutError(description)


def as_user(net, *args):
    return [
        "sudo",
        "-n",
        "ip",
        "netns",
        "exec",
        net,
        "taskset",
        "-c",
        str(CPU),
        "setpriv",
        "--reuid",
        str(os.getuid()),
        "--regid",
        str(os.getgid()),
        "--clear-groups",
        "--no-new-privs",
        "--inh-caps=-all",
        "--ambient-caps=-all",
        "--bounding-set=-all",
        *map(str, args),
    ]


def make_seed(path, schema):
    con = sqlite3.connect(path)
    con.executescript(schema.read_text())
    con.execute('insert into users(id,username) values(1,"reader")')
    con.execute(
        'insert into posts(id,user_id,body,created_at) values(1,1,"memory probe","2026-11-01T00:00:00.000Z")'
    )
    con.execute(
        """with recursive n(i) as (select 2 union all select i+1 from n where i<72000)
                   insert into posts select i,1,substr(hex(zeroblob(245)),1,480),'2026-10-01T00:00:00.000Z' from n"""
    )
    con.commit()
    con.close()


def setup_route(marker, names):
    sname = marker + "s"
    cname = marker + "c0"
    alloc = marker + "p"
    base.priv("ip", "netns", "add", alloc)
    names.append(alloc)
    base.ns(alloc, "sysctl", "-qw", "net.ipv4.tcp_child_ehash_entries=4096")
    for ns in (sname, cname):
        base.priv("nsenter", f"--net=/run/netns/{alloc}", "--", "ip", "netns", "add", ns)
        names.append(ns)
        require(base.ns(ns, "sysctl", "-n", "net.ipv4.tcp_ehash_entries") == "4096", "private ehash")
    sd = "sv" + marker[2:]
    cd = "cl" + marker[2:]
    base.priv(
        "ip",
        "-n",
        sname,
        "link",
        "add",
        sd,
        "numtxqueues",
        "1",
        "numrxqueues",
        "1",
        "type",
        "veth",
        "peer",
        "name",
        cd,
        "numtxqueues",
        "1",
        "numrxqueues",
        "1",
        "netns",
        cname,
    )
    nap = {}
    for ns, dev, addr in [(sname, sd, "198.18.203.1/30"), (cname, cd, "198.18.203.2/30")]:
        base.ip(ns, "link", "set", "lo", "up")
        base.ns(ns, "sysctl", "-qw", f"net.ipv6.conf.{dev}.disable_ipv6=1")
        base.ns(ns, "ethtool", "-K", dev, "gro", "on", "gso", "on", "tso", "off")
        base.ip(ns, "addr", "add", addr, "dev", dev)
        base.ip(ns, "link", "set", "dev", dev, "mtu", "1500", "up")
        nap[dev] = json.loads(
            base.ns(
                ns,
                "/usr/bin/python3",
                HERE.parent / "threaded-napi" / "private_napi.py",
                dev,
                CPU,
                Path("/run/netns", ns).stat().st_ino,
                Path("/proc/self/ns/net").stat().st_ino,
            )
        )
        require(all(x["cpus"] == [CPU] for x in nap[dev]["napi"]), "NAPI pin")
    return nap


def run(a):
    require(
        os.getuid() != 0 and os.sched_getaffinity(0) == {CPU},
        "proof requires the ordinary user on leased CPU 9",
    )
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    marker = "tr" + uuid.uuid4().hex[:7]
    sname = marker + "s"
    cname = marker + "c0"
    alloc = marker + "p"
    sl, unit = private_paths(marker)
    require(not (CG / sl).exists(), "fresh unique scope")
    names = []
    manager = client = None
    started = False
    with tempfile.TemporaryDirectory(prefix="private-cap-", dir=a.work) as dirname:
        work = Path(dirname)
        seed = work / "source.db"
        service = work / "service"
        service.mkdir()
        make_seed(seed, a.schema)
        try:
            nap = setup_route(marker, names)
            app = APP // 64
            guest = GUEST // 64
            props = [
                f"User={os.getuid()}",
                f"Group={os.getgid()}",
                "NoNewPrivileges=yes",
                "LimitNOFILE=65535:65535",
                f"MemoryMax={app}",
                "MemorySwapMax=0",
                f"CPUAffinity={CPU}",
                f"AllowedCPUs={CPU}",
                f"NetworkNamespacePath=/run/netns/{sname}",
                "CapabilityBoundingSet=",
                "AmbientCapabilities=",
                f"Slice={sl}",
                f"StandardError=append:{service}/stderr",
                f"StandardOutput=append:{service}/stdout",
            ]
            env = {"HOST": "0.0.0.0", "PORT": str(PORT), "JWT_SECRET": "twelve-dollar-challenge"}
            base.priv(
                "systemd-run",
                "--quiet",
                "--collect",
                "--unit=" + unit,
                *["--property=" + p for p in props],
                *[f"--setenv={k}={v}" for k, v in env.items()],
                "/usr/bin/python3",
                HERE.parent / "router" / "launch.py",
                service,
                seed,
                a.server,
            )
            started = True
            wait_for(
                lambda: (service / "stderr").exists()
                and "socket admission" in (service / "stderr").read_text(),
                "guarded service readiness: " + str(service),
                30,
            )
            pid = json.loads((service / "app-start.json").read_text())["pid"]
            manager = Manager(out, marker, pid, a.server, CPU, CPU, PORT, app, guest, 100)
            local = open(out / "proof-client-stderr.log", "w")
            client = subprocess.Popen(
                as_user(cname, "/usr/bin/python3", HERE / "proof_client.py"),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=local,
                text=True,
            )
            require(
                select.select([client.stdout], [], [], 5)[0]
                and client.stdout.readline().strip() == "READY",
                "client ready",
            )
            script_start = time.time()

            def act(cmd):
                client.stdin.write(cmd + "\n")
                client.stdin.flush()
                require(select.select([client.stdout], [], [], 30)[0], "client command timed out")
                response = client.stdout.readline()
                require(response, "client failed: " + (out / "proof-client-stderr.log").read_text()[-1800:])
                return json.loads(response)

            stages = []

            def stage(name, count):
                left = time.monotonic_ns()
                received = act("resize " + str(count))
                right = time.monotonic_ns()

                def observed():
                    for line in reversed((out / "passive-memory.jsonl").read_text().splitlines()):
                        try:
                            r = json.loads(line)
                        except ValueError:
                            continue
                        if (
                            r["kind"] == "sample"
                            and r["mono_ns"] >= right
                            and r["count"] == count
                            and r["cap"] == budget(app, guest, count)
                        ):
                            return r

                rec = wait_for(observed, "private socket count did not converge to " + str(count))
                stages.append(
                    dict(
                        stage=name,
                        expected=count,
                        correct=received,
                        total_transition_ms=(right - left) / 1e6,
                        observation_lag_ms=(rec["mono_ns"] - right) / 1e6,
                        sample=rec,
                    )
                )
                return rec

            stage("low", 128)
            stage("high", 2304)
            act("sample")
            write = act("write")
            time.sleep(0.35)
            stage("reduced", 128)
            stage("closed", 0)
            end = time.time()
            base.write(out / "timing.json", {"stock_start_unix": script_start, "cutoff_unix": end})
            client.stdin.write("quit\n")
            client.stdin.flush()
            require(client.wait(timeout=3) == 0, "client exit")
            base.priv("systemctl", "stop", unit)
            started = False
            require(manager.proc.wait(timeout=4) == 0, "manager did not notice the real service exit")
            manager.close()
            manager = None
            result = evaluate(out)
            require(
                not result["fixture_flags"] and result["exit_reason"] == "service_exit", "scored coverage"
            )
            require(
                not result["oom_kill_at_cutoff"] and all(r["sample"]["swap"] == 0 for r in stages),
                "unexpected OOM/swap",
            )
            require(
                stages[0]["sample"]["cap"] == app
                and stages[1]["sample"]["cap"] < app
                and stages[-1]["sample"]["cap"] == app,
                "parent cap did not tighten and recover",
            )
            base.write(out / "passive-memory-result.json", result)
            proof = dict(
                stages=stages,
                write_check=write,
                source_elf_sha256=base.sha(a.server),
                fixed_schema_sha256=base.sha(a.schema),
                scaled_app_bytes=app,
                scaled_guest_bytes=guest,
                scale="1/64 amounts; same actual 2376 B per socket",
                fresh_seed_bytes=seed.stat().st_size,
                private_napi=nap,
                manager=result,
                used_swap=False,
            )
        finally:
            if client and client.poll() is None:
                client.stdin.close()
                try:
                    client.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    client.terminate()
            if started:
                base.priv("systemctl", "stop", unit, check=False)
            if manager:
                manager.close()
            if (CG / sl).exists():
                base.priv("systemctl", "stop", sl)
            for ns in reversed(names):
                base.priv("ip", "netns", "del", ns)
        proof["private_slice_gone"] = not (CG / sl).exists()
        proof["private_namespaces_gone"] = not any((Path("/run/netns") / n).exists() for n in names)
        require(
            proof["private_slice_gone"] and proof["private_namespaces_gone"], "private resources remained"
        )
        base.write(out / "proof.json", proof)
        print(
            json.dumps(
                {
                    "stages": [
                        (x["stage"], x["expected"], x["sample"]["cap"], x["observation_lag_ms"])
                        for x in stages
                    ],
                    "coverage": result,
                    "private_slice_gone": proof["private_slice_gone"],
                }
            ),
            flush=True,
        )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "schema", "out", "work"):
        p.add_argument("--" + name, type=Path, required=True)
    run(p.parse_args())
