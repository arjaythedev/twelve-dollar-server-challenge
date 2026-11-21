"""Keep the split TAP packet path, steer service TCP via ordinary private-queue RPS."""
import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tap-ring4096"))
from run_buffered import BufferedSplitTap, base, route_proof

PROFILE = "single-queue-split-polling-tap-external-txring4096-ordinary-rps"
MANIFEST = ROOT / "manifest.json"


class OrdinaryRpsTap(BufferedSplitTap):
    def __init__(self, relay):
        super().__init__(relay)
        self.manifest = json.loads(MANIFEST.read_bytes())
        self.manifest_hash = base.sha(MANIFEST)
        self.verify_sources()

    def verify_sources(self):
        hashes = {name: base.sha(ROOT / name) for name in self.manifest["files"]}
        if hashes != self.manifest["files"] or base.sha(self.binary) != self.manifest["relay_sha256"]:
            raise RuntimeError("private RPS fixture sources or ordinary split relay differ from frozen manifest")
        if base.sha(MANIFEST) != self.manifest_hash:
            raise RuntimeError("private RPS manifest changed after launch")
        return hashes

    def read_queue(self):
        ns = self.service_namespace
        return json.loads(base.ns(ns, "/usr/bin/python3", "-I", "-B", HERE / "private_queue.py",
                                  self.interfaces[ns], self.service_cpu,
                                  os.stat(f"/run/netns/{ns}").st_ino, os.stat("/proc/self/ns/net").st_ino, "--read"))

    def sample(self, namespace):
        result = super().sample(namespace)
        result["private_service_receive"] = self.read_queue()
        return result

    def set_up(self, args, names, created):
        self.service_cpu = args.app_cpu
        if (self.service_cpu, args.manager_cpu, list(args.router_cpus), list(args.groups), args.delay_us,
                args.aliases, args.server_ehash) != (10, 15, [2, 5, 8, 14], ["0,1", "3,4", "6,7", "11,12,13"], 75, 8, 16384):
            raise ValueError("this separate cohort is defined on the primary single-service-CPU, four-generator geometry")
        data = super().set_up(args, names, created)
        cfg = data[names[1]]["polling_relay"]
        cfg["fixture_source_sha256_at_launch"] = self.verify_sources()
        cfg["source_manifest_sha256_at_launch"] = self.manifest_hash
        cfg["private_service_receive_after_wire"] = self.read_queue()
        cfg["scheduler_observation"].pop("private_service_napi")
        cfg["scheduler_observation"]["service_rx"] = "ordinary RX backlog on service CPU; /proc/stat softirq spans all work on that CPU"
        cfg["scheduler_observation"]["device_poll"] = "TAP NAPI/GRO on injection CPUs; separate archived single-flow poll traces"
        return data


if __name__ == "__main__":
    relay = Path(os.environ.get("TWELVE_SPLIT_TAP_RELAY", str(ROOT.parent.parent / "tap-split/relay")))
    fixture = OrdinaryRpsTap(relay)
    try:
        if "--wire-proof-only" in sys.argv:
            route_proof(fixture)
        else:
            base.main(network_setup=fixture.set_up, network_profile=PROFILE, network_sample=fixture.sample, delay_us=75)
    finally:
        fixture.close()
