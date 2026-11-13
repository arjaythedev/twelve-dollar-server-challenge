"""Ring4096 under a separate private upper bound for missing passive TCP slab."""

import json
import math
import os
from pathlib import Path
import select
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from monitor import APP, CADENCE_MS, CG, GUEST, PER_SOCKET, private_paths

sys.path.insert(0, str(HERE.parent / "tap-ring4096"))
from run_buffered import BufferedSplitTap, base

PROFILE = "single-queue-split-polling-tap-external-txring4096-private-passive-tcp-memory"
SOURCES = {
    p.name: base.sha(p) for p in (HERE / "monitor.py", HERE / "run_memory.py", HERE / "calibration.json")
}


class Manager:
    def __init__(self, out, marker, pid, exe, cpu, app_cpu, port, app=APP, guest=GUEST, cadence=CADENCE_MS):
        self.out = out
        self.log = (out / "passive-memory.jsonl").open("x")
        cmd = [
            "sudo",
            "-n",
            "taskset",
            "-c",
            str(cpu),
            "/usr/bin/python3",
            "-I",
            "-B",
            str(HERE / "monitor.py"),
            "--marker",
            marker,
            "--pid",
            str(pid),
            "--exe",
            str(exe),
            "--cpu",
            str(cpu),
            "--app-cpu",
            str(app_cpu),
            "--port",
            str(port),
            "--app",
            str(app),
            "--guest",
            str(guest),
            "--cadence",
            str(cadence),
        ]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.PIPE, text=True
        )
        try:
            if (
                not select.select([self.proc.stderr], [], [], 10)[0]
                or self.proc.stderr.readline().strip() != "READY"
            ):
                if self.proc.poll() is not None:
                    why = self.proc.stderr.read()
                else:
                    why = "monitor did not confirm its first cap"
                raise RuntimeError(why)
        except BaseException:
            self.close()
            raise

    def close(self):
        if not self.proc.stdin.closed:
            self.proc.stdin.close()
        try:
            self.proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(timeout=4)
        self.log.close()
        errors = self.proc.stderr.read()
        self.proc.stderr.close()
        if self.proc.returncode:
            raise RuntimeError(f"private memory monitor failed (exit {self.proc.returncode}): {errors}")


def quantile(values, q):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values) * q) - 1)]


def evaluate(out):
    rows = [json.loads(s) for s in (out / "passive-memory.jsonl").read_text().splitlines()]
    identity = rows[0]
    samples = [r for r in rows if r["kind"] == "sample"]
    if identity["kind"] != "identity" or not samples or rows[-1]["kind"] != "end":
        raise RuntimeError("missing required memory monitor evidence")
    timing = json.loads((out / "timing.json").read_text())
    start = timing["stock_start_unix"] * 1e9
    cut = timing["cutoff_unix"] * 1e9
    scored = [r for r in samples if start <= r["unix_ns"] <= cut]
    before = [r for r in samples if r["unix_ns"] <= cut]
    if not before:
        raise RuntimeError("no memory control observation before cutoff")
    exit_record = rows[-1]
    oom = max(
        [r["events"].get("oom_kill", 0) for r in before]
        + ([exit_record.get("events", {}).get("oom_kill", 0)] if exit_record["unix_ns"] <= cut else [])
    )
    gaps = [r["interval_ns"] / 1e6 for r in scored]
    seen_exit = exit_record["reason"] == "service_exit" and exit_record["unix_ns"] <= cut
    flags = []
    if exit_record["reason"] == "driver_closed" and exit_record["unix_ns"] < cut:
        flags.append("driver stopped the private memory manager before the scoring cutoff")
    if samples[0]["unix_ns"] > start:
        flags.append("memory control began after the organizer clock")
    # A genuine service exit is a service outcome. A live service with a missing trace is a fixture failure.
    if not seen_exit and (not scored or cut - before[-1]["unix_ns"] > 1e9):
        flags.append("private memory control did not cover the scoring cutoff")
    if max(gaps, default=0) > 1000:
        flags.append("private memory controller had a pre-cutoff observation gap above 1s")
    all_events = [r["events"] for r in before]
    return dict(
        profile=PROFILE,
        sources=SOURCES,
        identity=identity,
        records=len(samples),
        scored_records=len(scored),
        first_sample_unix=samples[0]["unix_ns"] / 1e9,
        cutoff_unix=cut / 1e9,
        last_pre_cutoff_unix=before[-1]["unix_ns"] / 1e9,
        app_exited_before_cutoff=seen_exit,
        exit_reason=exit_record["reason"],
        oom_kill_at_cutoff=oom,
        fixture_flags=flags,
        scored_peak_live_ipv4=max((r["count"] for r in scored), default=0),
        scored_min_parent_limit_bytes=min(
            (r["cap"] for r in scored), default=identity["original_leaf_max"]
        ),
        scored_peak_current_plus_modeled_bytes=max(
            (r["current"] + r["untracked_bytes"] for r in scored), default=0
        ),
        max_before_cutoff_events={k: max(z[k] for z in all_events) for k in all_events[0]},
        p99_sample_gap_ms=quantile(gaps, 0.99),
        max_sample_gap_ms=max(gaps, default=0),
        max_sampled_growth_bytes=max((max(0, r["count_change"]) * PER_SOCKET for r in scored), default=0),
        p99_write_ms=quantile([r["write_ns"] / 1e6 for r in scored if r["changed"]], 0.99),
        p99_cycle_ms=quantile([r["cycle_ns"] / 1e6 for r in scored], 0.99),
        manager_process_cpu_s=(samples[-1]["manager_cpu_ns"] - samples[0]["manager_cpu_ns"]) / 1e9,
        parent_resets_when_private_slice_disappears=True,
    )


class MemoryFixture(BufferedSplitTap):
    def __init__(self, relay):
        super().__init__(relay)
        self.properties = []
        self.manager = None
        self.slice = None
        self.marker = None

    def set_up(self, args, names, created):
        self.marker = names[0][:-1]
        sl, unit = private_paths(self.marker)
        if (CG / sl).exists():
            raise RuntimeError("proposed fresh private slice already exists")
        self.slice = sl
        self.properties[:] = ["Slice=" + sl]
        topo = super().set_up(args, names, created)
        meta = topo[names[1]]["polling_relay"]
        meta["fixture_source_sha256_at_launch"].update({"tap-memory/" + k: v for k, v in SOURCES.items()})
        meta["passive_tcp_memory"] = {
            "outer_slice": sl,
            "leaf_unit": unit,
            "leaf_limit": APP,
            "leaf_swap_max": 0,
            "upper_envelope_bytes": GUEST,
            "missing_bytes_per_socket": PER_SOCKET,
            "mode": "upper bound on available guest RAM, excludes OS and global TCP pressure",
            "counter": "namespace IPv4 TCP inuse, less its validated IPv4 :80 listeners",
            "cadence_ms": CADENCE_MS,
            "calibration": json.loads((HERE / "calibration.json").read_text()),
        }
        return topo

    def inspect(self, cg, args):
        data = json.loads((self.out / "app-start.json").read_text())
        sl, unit = private_paths(self.marker)
        if cg != CG / sl / unit or data["uid"] != os.getuid() or data["affinity"] != [args.app_cpu]:
            raise RuntimeError("launcher/cgroup/CPU identity mismatch")
        self.manager = Manager(
            self.out, self.marker, data["pid"], args.server, args.manager_cpu, args.app_cpu, 80
        )
        return dict(
            memory_profile=PROFILE,
            service_cgroup=str(cg),
            manager_cpu=args.manager_cpu,
            original_leaf_memory_max=APP,
            original_leaf_swap_max=0,
            source_sha256=SOURCES,
        )

    def sample(self, namespace):
        if self.manager and self.manager.proc.poll() not in (None, 0):
            raise RuntimeError("private memory manager exited during the trial")
        return super().sample(namespace)

    def finish_memory(self):
        if not self.manager:
            return
        try:
            self.manager.close()
            evidence = evaluate(self.out)
        except Exception as error:
            evidence = dict(
                profile=PROFILE,
                sources=SOURCES,
                fixture_flags=[str(error)],
                oom_kill_at_cutoff=0,
                app_exited_before_cutoff=False,
            )
        base.write(self.out / "passive-memory-result.json", evidence)
        path = self.out / "summary.json"
        if path.exists():
            row = json.loads(path.read_text())
            row["passive_tcp_memory_status"] = (
                "fixture-failure"
                if evidence["fixture_flags"]
                else (
                    "service-oom"
                    if evidence["oom_kill_at_cutoff"]
                    else "service-exit" if evidence["app_exited_before_cutoff"] else "covered"
                )
            )
            row["passive_memory_result"] = "passive-memory-result.json"
            row["oom_kill_at_cutoff"] = max(
                row.get("oom_kill_at_cutoff", 0), evidence["oom_kill_at_cutoff"]
            )
            if row["passive_tcp_memory_status"] != "covered":
                row["eligible"] = row["qualified"] = False
            if row["oom_kill_at_cutoff"]:
                row["wire_eligible"] = row["scored_healthy_environment"] = False
            if evidence["fixture_flags"]:
                row["scored_healthy_environment"] = False
                row.setdefault("infrastructure_flags", []).extend(evidence["fixture_flags"])
            base.write(path, row)
        if evidence["fixture_flags"]:
            raise RuntimeError(evidence["fixture_flags"])

    def run(self):
        try:
            base.main(
                network_setup=self.set_up,
                network_profile=PROFILE,
                network_sample=self.sample,
                delay_us=75,
                service_properties=self.properties,
                inspect_service=self.inspect,
            )
        finally:
            try:
                self.finish_memory()
            finally:
                try:
                    self.close()
                finally:
                    if self.slice and (CG / self.slice).exists():
                        base.priv("systemctl", "stop", self.slice)
                    if self.slice and (CG / self.slice).exists():
                        raise RuntimeError("private trial slice remained")


if __name__ == "__main__":
    relay = Path(os.environ.get("TWELVE_SPLIT_TAP_RELAY", str(HERE.parent / "tap-split" / "relay")))
    MemoryFixture(relay).run()
