"""Ordinary TCP with per-lane server RX and one fixed, approximate guest process CPU budget."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import pwd
import signal
import sys
import tempfile
import time
from types import SimpleNamespace
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "router"))
import run as base
from socket_probe import SIZES


PROFILE = "ordinary-tcp-remote-rx-82pct-approximation"
ANCHORS = {
    "willwang": {"users": 280000, "app_cpu_pct": 82, "report": "https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/403#issuecomment-6816178650"},
    "kensac": {"users": 310000, "app_cpu_pct": 82, "report": "https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264#issuecomment-6566431071"},
    "mojo": {"users": 304000, "app_cpu_pct": 82, "report": "https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/257#issuecomment-6564516869"},
}
PERIOD_US = 5000
CPU_PCT = sum(anchor["app_cpu_pct"] for anchor in ANCHORS.values()) // len(ANCHORS)
QUOTA_US = PERIOD_US * CPU_PCT // 100
PROPERTIES = (f"CPUQuota={CPU_PCT}%", f"CPUQuotaPeriodSec={PERIOD_US}us")
COUNTERS = ("usage_usec", "user_usec", "system_usec", "nr_periods", "nr_throttled", "throttled_usec")


def inspect_service(cg, args):
    expected = {
        "cpu.max": f"{QUOTA_US} {PERIOD_US}", "cpu.max.burst": "0",
        "cpuset.cpus.effective": str(args.app_cpu), "memory.max": str(1750 * 1024**2),
        "memory.swap.max": "0",
    }
    observed = {name: (cg / name).read_text().strip() for name in expected}
    if expected != observed:
        raise RuntimeError(f"CPU/memory control mismatch: expected {expected}, observed {observed}")
    stat = base.memstats(cg)["cpu.stat"]
    missing = set(COUNTERS).difference(stat)
    if missing:
        raise RuntimeError(f"missing cgroup v2 CPU statistics: {missing}")
    return {
        "profile": PROFILE, "published_anchors": ANCHORS,
        "requested_systemd_properties": list(PROPERTIES), "cgroup": str(cg),
        "actual": observed, "cpu.stat": stat,
    }


def read(path):
    return json.loads(path.read_text())


def wait_json(path, unit):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.exists():
            return read(path)
        if base.run("systemctl", "is-active", unit, check=False) not in {"active", "activating"}:
            raise RuntimeError(f"TCP probe exited before {path.name}: {(path.parent / 'service.log').read_text()}")
        time.sleep(0.05)
    raise TimeoutError(f"timed out awaiting {path.name}: {(path.parent / 'service.log').read_text()}")


def route(namespace, destination, expected_dev, expected_gateway):
    routes = json.loads(base.ip(namespace, "-j", "route", "get", destination))
    if len(routes) != 1 or (routes[0]["dev"], routes[0].get("gateway")) != (expected_dev, expected_gateway):
        raise RuntimeError(f"unexpected route from {namespace} to {destination}: {routes}")
    return routes[0]


def network_delta(before, after, check_path=True):
    result = {}
    expected = {f"{side}{index}" for side in ("s", "c") for index in range(1, 5)}
    for group in ("router_rx", "router_tx", "router_queues"):
        if set(before[group]) != expected or set(after[group]) != expected:
            raise RuntimeError(f"missing external interfaces in {group}")
        result[group] = {}
        for device, later in after[group].items():
            earlier = before[group][device]
            keys = ("packets", "drops", "overlimits", "requeues") if group == "router_queues" else ("packets", "dropped", "errors")
            changes = {k: later[k] - earlier[k] for k in keys}
            if check_path and (changes["packets"] <= 0 or any(changes.get(k, 0) for k in ("drops", "errors", "dropped"))):
                raise RuntimeError(f"external path did not forward cleanly at {group}/{device}: {changes}")
            result[group][device] = changes
    return result


def stat_delta(earlier, later, elapsed):
    delta = {key: later[key] - earlier[key] for key in COUNTERS}
    if elapsed <= 0 or min(delta.values()) < 0:
        raise ValueError(f"non-monotonic cgroup observation: {delta}, {elapsed}")
    return {
        "elapsed_s": elapsed, "cpu.stat_delta": delta,
        "app_pct_of_one_cpu": 100 * delta["usage_usec"] / (elapsed * 1e6),
        "throttled_periods_pct": 100 * delta["nr_throttled"] / delta["nr_periods"] if delta["nr_periods"] else None,
        "throttled_usec_per_wall_second": delta["throttled_usec"] / elapsed,
    }


def placement(args, names, out):
    server, router, *clients = names
    unit = f"twelve-rx-budget-proof-{server}.service"
    out.mkdir()
    # Do not run a candidate or the official client during the placement check.
    props = [
        f"User={pwd.getpwuid(os.getuid()).pw_name}", "NoNewPrivileges=yes",
        "CPUAccounting=yes", f"CPUAffinity={args.app_cpu}", f"AllowedCPUs={args.app_cpu}",
        "MemoryMax=1750M", "MemorySwapMax=0", f"NetworkNamespacePath=/run/netns/{server}",
        f"StandardOutput=append:{out}/service.log", f"StandardError=append:{out}/service.log", *PROPERTIES,
    ]
    routes = {}
    for i, client in enumerate(clients, 1):
        routes[f"lane{i}"] = {
            "from_client": route(client, f"10.232.{i}.10", "eth0", f"10.233.{i}.1"),
            "to_client": route(server, f"10.233.{i}.2", f"eth{i}", f"10.232.{i}.1"),
            "router_to_server": route(router, f"10.232.{i}.10", f"s{i}", None),
            "router_to_client": route(router, f"10.233.{i}.2", f"c{i}", None),
        }
    base.write(out / "routes.json", routes)
    base.priv("systemd-run", "--quiet", "--collect", f"--unit={unit}",
              *[f"--property={prop}" for prop in props], "/usr/bin/python3",
              HERE / "socket_probe.py", "server", out, len(clients) * 2)
    try:
        ready = wait_json(out / "ready.json", unit)
        if ready["affinity"] != [args.app_cpu] or ready["uid"] != os.getuid():
            raise RuntimeError(f"bad probe receiver affinity/user: {ready}")
        group = base.run("systemctl", "show", unit, "--property=ControlGroup", "--value")
        if not group or ready["cgroup"] != f"0::{group}":
            raise RuntimeError(f"TCP receiver cgroup mismatch: {ready}, {group}")
        cg = Path("/sys/fs/cgroup") / group.lstrip("/")
        controls = inspect_service(cg, args)
        base.write(out / "service-controls.json", controls)
        network_before = base.router_network_sample(router)
        t0 = time.monotonic()
        cpu_before = base.memstats(cg)["cpu.stat"]
        for i, (client, group_cpus) in enumerate(zip(clients, args.groups), 1):
            cpus = group_cpus.split(",")
            for suffix, cpu in zip((10, 10 + args.aliases - 1), (cpus[0], cpus[-1])):
                base.ns(client, "setpriv", f"--reuid={os.getuid()}", f"--regid={os.getgid()}",
                        "--init-groups", "--inh-caps=-all", "--ambient-caps=-all", "--bounding-set=-all",
                        "--no-new-privs", "/usr/bin/python3", HERE / "socket_probe.py", "client",
                        f"10.232.{i}.{suffix}", cpu)
        seen = wait_json(out / "received.json", unit)
        cpu_after = base.memstats(cg)["cpu.stat"]
        elapsed = time.monotonic() - t0
        network_after = base.router_network_sample(router)
        validation = {}
        for i, lane_cpu in enumerate(args.router_cpus, 1):
            rows = [row for row in seen if row["peer"] == f"10.233.{i}.2"]
            expected_addrs = sorted(f"10.232.{i}.{n}" for n in (10, 10 + args.aliases - 1))
            if len(rows) != 2 or sorted(row["local"] for row in rows) != expected_addrs:
                raise RuntimeError(f"incorrect forward path for lane {i}: {rows}")
            sizes = [sample["bytes"] for row in rows for sample in row["samples"]]
            observations = [sample["incoming_cpu"] for row in rows for sample in row["samples"]]
            if sizes != list(SIZES) * 2 or observations != [lane_cpu] * (len(SIZES) * 2):
                raise RuntimeError(f"unexpected socket RX placement on lane {i}, expected CPU {lane_cpu}: {rows}")
            validation[f"eth{i}"] = {"expected_cpu": lane_cpu, "observed_cpus": observations,
                                     "destination_aliases": expected_addrs}
        result = {
            "app": ready, "cgroup_controls": controls, "tcp_receive_cpu_proof": validation,
            "router_counter_deltas": network_delta(network_before, network_after),
            "cgroup_counter_window": stat_delta(cpu_before, cpu_after, elapsed),
        }
        base.write(out / "proof.json", result)
        return result
    finally:
        (out / "release").touch()
        base.priv("systemctl", "stop", unit, check=False)


def physical_cores(args):
    roles = {args.app_cpu: "app", args.manager_cpu: "manager"}
    roles.update({cpu: f"router-{lane}" for lane, cpu in enumerate(args.router_cpus, 1)})
    for lane, group in enumerate(args.groups, 1):
        roles.update({int(cpu): f"generator-{lane}" for cpu in group.split(",")})
    cpus, owners = {}, {}
    for cpu, role in roles.items():
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        package = int((path / "physical_package_id").read_text())
        core = int((path / "core_id").read_text())
        previous = owners.setdefault((package, core), role)
        dedicated = previous == "app" or role == "app" or previous.startswith("router-") or role.startswith("router-")
        if previous != role and dedicated:
            raise RuntimeError(f"{role} and {previous} share physical core {package}/{core}")
        cpus[cpu] = {"role": role, "socket": package, "core": core}
    return cpus


def setup(args, names, created, natural=False):
    # Choose one fixed policy for *every* comparison. A native same-lane veth
    # delivery already matches this single-bit mask and needs no remote RPS IPI.
    masks = ([0] * len(args.router_cpus) if natural else [1 << cpu for cpu in args.router_cpus])
    cores = physical_cores(args)
    topology = base.set_up(args, names, created, server_rx_masks=masks)
    observed = placement(args, names, args.output.resolve() / args.tag / "rx-placement")
    topology[names[0]]["ordinary_tcp_remote_rx"] = {
        "estimated_process_budget_pct": CPU_PCT, "natural_no_rps_probe": natural,
        "physical_cpu_roles": cores,
        "cpu_by_server_device": {f"eth{i}": c for i, c in enumerate(args.router_cpus, 1)},
        "placement": observed["tcp_receive_cpu_proof"],
    }
    return topology


def summarize(out):
    telemetry = out / "samples.jsonl"
    if not telemetry.is_file():
        return
    samples = [json.loads(line) for line in telemetry.read_text().splitlines()]
    timing = read(out / "timing.json")
    ranges = {
        "ramp": (timing["stock_start_unix"], timing["hold_starts_unix"]),
        "hold": (timing["hold_starts_unix"], timing["cutoff_unix"]),
        "through_cutoff": (timing["stock_start_unix"], timing["cutoff_unix"]),
    }
    report = {"profile": PROFILE, "actual": read(out / "service-controls.json")["actual"], "windows": {}}
    for name, (lo, hi) in ranges.items():
        selected = [row for row in samples if lo <= row["unix"] <= hi]
        if len(selected) < 2:
            report["windows"][name] = {"samples": len(selected), "reason": "insufficient in-window coverage"}
            continue
        start, end = selected[0], selected[-1]
        report["windows"][name] = {
            "samples": len(selected), "first_unix": start["unix"], "last_unix": end["unix"],
            **stat_delta(start["app"]["cpu.stat"], end["app"]["cpu.stat"], end["unix"] - start["unix"]),
        }
    network = out / "network-samples.jsonl"
    if network.is_file():
        samples = [json.loads(line) for line in network.read_text().splitlines()]
        samples = [s for s in samples if s["unix"] < timing["cutoff_unix"]]
        if len(samples) >= 2:
            first, last = samples[0], samples[-1]
            changes = network_delta(first, last, check_path=False)
            errors = {}
            for group in ("router_rx", "router_tx", "router_queues"):
                for device, stats in last[group].items():
                    failed = {key: stats[key] for key in ("errors", "dropped", "drops") if stats.get(key)}
                    if failed:
                        errors.setdefault(group, {})[device] = failed
            report["pre_cutoff_router"] = {
                "first_unix": first["unix"], "last_unix": last["unix"],
                "seconds_between_last_sample_and_cutoff": timing["cutoff_unix"] - last["unix"],
                "traffic_delta": changes,
                "drops_or_errors_since_private_interfaces_created": errors,
            }
    base.write(out / "budget-telemetry.json", report)


def run():
    chosen = {}

    def checked_setup(args, names, created):
        chosen["out"] = args.output.resolve() / args.tag
        return setup(args, names, created)

    try:
        base.main(network_setup=checked_setup, network_profile=PROFILE,
                  network_sample=base.router_network_sample, service_properties=PROPERTIES, inspect_service=inspect_service)
    finally:
        if "out" in chosen:
            summarize(chosen["out"])


def proof():
    parser = argparse.ArgumentParser(description="Validate the private paths at low rate without running mload.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--natural", action="store_true", help="turn off private server RPS for a locality-only proof")
    options = parser.parse_args(sys.argv[2:])
    if os.getuid() == 0:
        parser.error("run as the ordinary benchmark user")
    folder = options.output.resolve()
    args = SimpleNamespace(output=folder.parent, tag=folder.name, server_ehash=16384, delay_us=48, aliases=8,
                           app_cpu=base.APP_CPU, router_cpus=list(base.ROUTER_CPUS), groups=list(base.GROUPS),
                           users=4, manager_cpu=base.MANAGER_CPU)
    mutex_path = Path(tempfile.gettempdir()) / f"twelve-router-{os.getuid()}.lock"
    mutex = os.open(mutex_path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    fcntl.flock(mutex, fcntl.LOCK_EX)
    os.sched_setaffinity(0, {args.manager_cpu})
    folder.mkdir(parents=True, exist_ok=False)
    prefix = "rb" + uuid.uuid4().hex[:7]
    names = [prefix + s for s in ("s", "r", "c0", "c1", "c2", "c3")]
    created = []

    def finish(n, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, finish)
    try:
        topology = setup(args, names, created, natural=options.natural)
        base.write(folder / "setup.json", {
            "profile": PROFILE, "natural_no_rps_probe": options.natural, "scored": False,
            "namespaces": names, "network": topology, "fixed_cpu_budget_pct": CPU_PCT,
            "probe_connections": 8, "payload_receives": 8 * len(SIZES), "mload_started": False,
        })
        print(json.dumps({"profile": PROFILE, "proof": str(folder), "result": "passed", "scored": False}), flush=True)
    finally:
        for name in reversed(created):
            base.priv("ip", "netns", "del", name, check=False)
        remaining = set(base.priv("ip", "netns", "list").split())
        survived = [name for name in created if name in remaining]
        base.write(folder / "teardown.json", {"created": created, "remaining": survived})
        os.close(mutex)
        if survived:
            raise RuntimeError(f"private fixture namespaces survived cleanup: {survived}")


if __name__ == "__main__":
    if sys.argv[1:2] == ["proof"]:
        proof()
    else:
        run()
