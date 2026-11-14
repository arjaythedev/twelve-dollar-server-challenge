"""Separate sensitivity profile with a larger bounded hardware-facing queue on each private TAP."""

import json
import os
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tap-split"))
from run_split import SplitTap, base, route_proof

PROFILE = "single-queue-split-polling-tap-external-txring4096"
LIMIT = 4096
SOURCE_SHA256_AT_LAUNCH = base.sha(Path(__file__))


class BufferedSplitTap(SplitTap):
    def start(self, cpu, delay, endpoints):
        super().start(cpu, delay, endpoints)
        self.ring_geometry = {}
        for ns in endpoints:
            raw = base.ns(ns, "ip", "-j", "-d", "link", "show", "dev", "labtap")
            (link,) = json.loads(raw)
            if (
                link["linkinfo"]["info_kind"] != "tun"
                or link["linkinfo"]["info_data"]["type"] != "tap"
                or link["num_tx_queues"] != 1
                or link["num_rx_queues"] != 1
                or link["txqlen"] != 1000
            ):
                raise RuntimeError(f"unexpected unmodified private TAP geometry in {ns}")
            base.ip(ns, "link", "set", "dev", "labtap", "txqueuelen", LIMIT)
            (after,) = json.loads(base.ns(ns, "ip", "-j", "-d", "link", "show", "dev", "labtap"))
            if after["txqlen"] != LIMIT or after["ifindex"] != link["ifindex"]:
                raise RuntimeError(f"private TAP transmit-ring resize failed in {ns}")
            self.ring_geometry[ns] = {"ifindex": link["ifindex"], "new": LIMIT, "original": link["txqlen"]}

    def set_up(self, args, names, created):
        topology = super().set_up(args, names, created)
        cfg = topology[names[1]]["polling_relay"]
        cfg["fixture_source_sha256_at_launch"] = {
            **cfg["fixture_source_sha256_at_launch"],
            "tap-ring4096/run_buffered.py": SOURCE_SHA256_AT_LAUNCH,
        }
        cfg["private_tap_transmit_ring_geometry"] = self.ring_geometry
        if any(topology[name]["interface"]["txqlen"] != LIMIT for name in self.ring_geometry):
            raise RuntimeError("private TAP ring changed between setup and byte-checked preflight")
        return topology


if __name__ == "__main__":
    relay = Path(os.environ.get("TWELVE_SPLIT_TAP_RELAY", str(HERE.parent / "tap-split" / "relay")))
    fixture = BufferedSplitTap(relay)
    try:
        if "--wire-proof-only" in sys.argv:
            route_proof(fixture)
        else:
            base.main(network_setup=fixture.set_up, network_profile=PROFILE, network_sample=fixture.sample,
                      delay_us=75)
    finally:
        fixture.close()
