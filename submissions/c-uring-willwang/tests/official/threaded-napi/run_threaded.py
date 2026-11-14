"""Opt-in local benchmark profile. Each private server veth receives through its verified NAPI kthread."""

import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "router"))
import run as base


profile = "four-router-lanes-threaded-napi"


def probe_command(name, cpus, side, address):
    return [
        "sudo",
        "-n",
        "ip",
        "netns",
        "exec",
        name,
        "taskset",
        "-c",
        str(cpus),
        "setpriv",
        f"--reuid={os.getuid()}",
        f"--regid={os.getgid()}",
        "--init-groups",
        "--inh-caps=-all",
        "--ambient-caps=-all",
        "--bounding-set=-all",
        "--no-new-privs",
        "/usr/bin/python3",
        str(HERE / "socket_probe.py"),
        side,
        profile,
        address,
        "4",
    ]


def verify_wire(server, client, app_cpu, client_cpus, address, records):
    app = subprocess.Popen(
        probe_command(server, app_cpu, "server", address),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        if not select.select([app.stdout], [], [], 4)[0] or app.stdout.readline().strip() != "READY":
            raise RuntimeError("private TCP placement probe did not start")
        subprocess.run(
            probe_command(client, client_cpus, "client", address),
            timeout=4,
            check=True,
            capture_output=True,
            text=True,
        )
        stdout, stderr = app.communicate(timeout=4)
        if app.returncode:
            raise RuntimeError(f"private placement probe failed: {stderr}")
        seen = json.loads(stdout)["observed_cpu_and_napi_id"]
        valid = {row["id"] for row in records}
        if len(seen) != 4 or any(cpu != app_cpu or napi_id not in valid for cpu, napi_id in seen):
            raise RuntimeError(
                f"RX did not stay on its application CPU and allocated NAPI IDs: {seen}, wanted {valid}"
            )
        return seen
    finally:
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=2)


def still_alive(row):
    try:
        return Path(f"/proc/{row['pid']}/stat").read_text().rsplit(")", 1)[1].split()[19] == row["starttime"]
    except FileNotFoundError:
        return False


def main():
    cleanup = {"tasks": []}

    def set_threaded(args, names, created):
        observations = base.set_up(args, names, created)
        cleanup["out"] = args.output.resolve() / args.tag
        server, router, *clients = names
        private_inode = os.stat(f"/run/netns/{server}").st_ino
        host_inode = os.stat("/proc/self/ns/net").st_ino
        obs = observations[server]
        obs["threaded_napi"] = {}
        for i, client in enumerate(clients, 1):
            dev, peer = f"eth{i}", f"s{i}"
            base.ns(router, "ethtool", "-K", peer, "tso", "off")
            features = base.ns(router, "ethtool", "-k", peer)
            flags = [
                s.strip()
                for s in features.splitlines()
                if s.strip().startswith(("tcp-segmentation-offload:", "tx-tcp"))
            ]
            if not flags or any(": on" in entry for entry in flags):
                raise RuntimeError(f"private router sender kept TCP segmentation offload enabled: {flags}")
            observations[router]["offloads"][peer]["tcp-segmentation-offload"] = "off"
            details = json.loads(
                base.ns(
                    server,
                    "/usr/bin/python3",
                    HERE / "private_napi.py",
                    dev,
                    args.app_cpu,
                    private_inode,
                    host_inode,
                )
            )
            cleanup["tasks"].extend(details["napi"])
            obs["rx_masks"][dev] = details["rx_rps_cpus"][0]
            details["router_peer_features"] = flags
            details["socket_probe_cpu_and_napi_id"] = verify_wire(
                server, client, args.app_cpu, args.groups[i - 1], f"10.232.{i}.10", details["napi"]
            )
            obs["threaded_napi"][dev] = details
        return observations

    try:
        base.main(network_setup=set_threaded, network_profile=profile)
    finally:
        if cleanup.get("out"):
            deadline = time.monotonic() + 6
            while any(still_alive(row) for row in cleanup["tasks"]) and time.monotonic() < deadline:
                time.sleep(0.05)
            survivors = [row for row in cleanup["tasks"] if still_alive(row)]
            base.write(
                cleanup["out"] / "threaded-cleanup.json",
                {"created_threads": len(cleanup["tasks"]), "unreclaimed_threads": survivors},
            )
            if survivors:
                raise RuntimeError(f"private namespace cleanup did not reap its NAPI threads: {survivors}")


if __name__ == "__main__":
    main()
