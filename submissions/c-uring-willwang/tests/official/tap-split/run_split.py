"""Separate guest TAP reads from generator-side frame delivery over ordered, bounded handoffs."""

import json
import os
from pathlib import Path
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tap-sharded"))
from run_sharded import ShardedTap, base, route_proof

PROFILE = "single-queue-split-polling-tap"
FIXTURE_SOURCES = (
    "tap-split/run_split.py",
    "tap-sharded/run_sharded.py",
    "tap-poll/run_tap.py",
    "tap-poll/check_flow.py",
    "tap-poll/tap_napi.py",
    "threaded-napi/run_threaded.py",
    "threaded-napi/socket_probe.py",
    "router/run.py",
    "router/measure.py",
    "router/assess.py",
    "router/launch.py",
)
SOURCE_SHA256_AT_LAUNCH = {name: base.sha(HERE.parent / name) for name in FIXTURE_SOURCES}


class SplitTap(ShardedTap):
    def sample(self, namespace):
        result = super().sample(namespace)
        device = self.interfaces[self.service_namespace]
        qdiscs = json.loads(base.ns(self.service_namespace, "tc", "-j", "-s", "qdisc", "show", "dev", device))
        result["service_tx_qdiscs"] = [
            {
                k: q[k]
                for k in (
                    "kind",
                    "handle",
                    "packets",
                    "drops",
                    "overlimits",
                    "requeues",
                    "backlog",
                    "qlen",
                    "drop_overlimit",
                )
                if k in q
            }
            for q in qdiscs
        ]
        scheduling = {}
        for worker in result["polling_relay"].get("workers", []):
            task = Path(f"/proc/{self.pid}/task/{worker['tid']}")
            try:
                run, wait, timeslices = map(int, (task / "schedstat").read_text().split()[:3])
                row = {"run_ns": run, "runqueue_wait_ns": wait, "timeslices": timeslices}
                for line in (task / "sched").read_text().splitlines():
                    key, sep, value = line.partition(":")
                    if sep and key.strip() in (
                        "nr_involuntary_switches",
                        "nr_voluntary_switches",
                        "se.statistics.wait_max",
                    ):
                        row[key.strip()] = float(value.strip())
                scheduling[str(worker["tid"])] = row
            except (FileNotFoundError, PermissionError):
                pass
        result["relay_worker_scheduling"] = scheduling
        return result

    def set_up(self, args, names, created):
        self.service_namespace = names[0]
        data = super().set_up(args, names, created)
        config = data[names[1]]["polling_relay"]
        config["fixture_source_sha256_at_launch"] = SOURCE_SHA256_AT_LAUNCH
        config["scheduler_observation"] = {
            "private_service_napi": "/proc/<napi-tid>/schedstat",
            "external_relay_workers": "/proc/<relay-pid>/task/<tid>/{schedstat,sched}",
            "application": "cpu.stat of the benchmark service cgroup",
            "application_cpu": "/proc/stat",
        }
        config["architecture"] = (
            "one guest TAP reader and per-generator single-producer/single-consumer reply queues"
        )
        if len(self.external_cpus) == 1:
            # The same split executable may use a combined worker in the optional spare-CPU smoke.
            config["single_cpu_proof"] = True
            return data
        deadline = time.monotonic() + 4
        while True:
            sample = self.sample(names[1])
            wire = sample["polling_relay"]
            handoffs = wire.get("handoffs", [])
            if (
                len(handoffs) == 4
                and all(h["delivered"] > 0 and h["written"] > 0 for h in handoffs)
                and wire["gso"] > 0
            ):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"private split queues did not all deliver the byte-checked flows: {handoffs}"
                )
            time.sleep(0.1)
        workers = wire["workers"]
        (guest_reader,) = [w for w in workers if w["sources"] == [0]]
        delivered_by = [(dst, w["cpu"]) for w in workers for dst in w.get("destinations", [])]
        owner = dict(delivered_by)
        if sorted(h["destination"] for h in handoffs) != [1, 2, 3, 4] or sorted(
            dst for dst, _ in delivered_by
        ) != [1, 2, 3, 4]:
            raise RuntimeError(f"private reply queues did not cover all destinations exactly once: {owner}")
        for h in handoffs:
            if (
                h["source"] != 0
                or h["producer_cpu"] != guest_reader["cpu"]
                or h["consumer_cpu"] != owner[h["destination"]]
                or h["consumer_cpu"] == h["producer_cpu"]
                or h["dropped"]
            ):
                raise RuntimeError(f"unexpected private reply handoff or pre-score loss: {h}")
        config["verified_handoffs"] = [
            {k: h[k] for k in ("source", "destination", "producer_cpu", "consumer_cpu")} for h in handoffs
        ]
        config["preflight_forwarding"] = {
            k: wire[k] for k in ("gso", "partial_checksum", "bad_frame", "queue_overflow")
        }
        config["service_tx_qdiscs"] = sample["service_tx_qdiscs"]
        config["workers_with_delivery_ownership"] = [
            {k: w.get(k, []) for k in ("tid", "cpu", "role", "sources", "destinations")} for w in workers
        ]
        return data


if __name__ == "__main__":
    fixture = SplitTap(Path(os.environ.get("TWELVE_SPLIT_TAP_RELAY", str(HERE / "relay"))))
    try:
        if "--wire-proof-only" in sys.argv:
            route_proof(fixture)
        else:
            base.main(
                network_setup=fixture.set_up,
                network_profile=PROFILE,
                network_sample=fixture.sample,
                delay_us=75,
            )
    finally:
        fixture.close()
