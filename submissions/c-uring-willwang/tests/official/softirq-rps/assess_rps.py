"""Run the strict common stock/health adjudication, then require this model's positive evidence."""
import hashlib
import json
from pathlib import Path
import sys

from assess import assess as assess_common, read_json, rows

ROOT = Path(__file__).resolve().parent.parent
PROFILE = "single-queue-split-polling-tap-external-txring4096-ordinary-rps"
EXPECTED_DIGEST = "d53e8b1caeda00a88770b99733b77a0f669ef0722d4fdbd168bdec428c939fad"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def queue_ok(row, cpu, reference):
    if not isinstance(row, dict):
        return False
    try:
        return (len(row["rx_rps_cpus"]) == 1 and int(row["rx_rps_cpus"][0].replace(",", ""), 16) == 1 << cpu
                and (row["threaded"], row["rps_flow_cnt"], row["napi"], row["socket_napi_id"]) == ("0", 0, [], 0)
                and all(row[k] == reference[k] for k in ("device", "ifindex", "namespace_inode")))
    except (KeyError, TypeError, ValueError):
        return False


def evidence(shape, timing, network, at_exit, measured, manifest, manifest_digest):
    errors, target = [], []
    facts = {}
    def check(condition, reason):
        if not condition:
            errors.append(reason)
    cpu = shape["app_cpu"]
    ns, manager, *clients = shape["namespaces"]
    topo = shape["network"]
    config = topo[manager]["polling_relay"]
    dev = config["server_tap"]
    server = topo[ns]
    check((shape["topology"], cpu, shape["manager_cpu"], shape["router_cpus"], shape["client_cpus"],
           shape["router_delay_us_each_direction"], shape["target_aliases"], shape["server_ehash_requested"]) ==
          (PROFILE, 10, 15, [2, 5, 8, 14], ["0,1", "3,4", "6,7", "11,12,13"], 75, 8, 16384),
          "private RPS cohort differs from the primary CPU, tuple, ring or delay geometry")
    check(config.get("source_manifest_sha256_at_launch") == manifest_digest and
          config["fixture_source_sha256_at_launch"] == manifest["files"] and
          config["binary_sha256"] == manifest["relay_sha256"] and shape["mload_sha256"] == manifest["stock_mload_sha256"],
          "private RPS source, split relay or organizer generator launch fingerprints differ")
    check(int(server["ehash_entries"]) == 16384 and not server.get("threaded_napi"), "server does not have the expected private hash and ordinary RX")
    ref = server["ordinary_rps"][dev]
    check(int(ref["original_mask"].replace(",", ""), 16) == 0 and queue_ok(ref, cpu, ref)
          and ref["ifindex"] == config["private_tap_transmit_ring_geometry"][ns]["ifindex"]
          and ref["device"] == dev and ref["namespace_inode"] > 0
          and int(server["rx_masks"][dev].replace(",", ""),16) == 1 << cpu,
          "initial private service TAP placement failed")
    check(queue_ok(config.get("private_service_receive_after_wire"), cpu, ref) and
          queue_ok(at_exit.get("private_service_receive"), cpu, ref), "post-wire or client-exit queue identity or RPS mask is absent or inconsistent")
    endpoints = [ns, *clients]
    check(len(clients) == 4 and config["ports"] == endpoints, "four external generator interfaces were not preserved")
    geometry = {"mtu":1500, "num_rx_queues":1, "num_tx_queues":1, "txqlen":4096}
    keys = {"tx-checksumming", "tcp-segmentation-offload", "generic-segmentation-offload", "generic-receive-offload"}
    for name in endpoints:
        row = topo[name]
        geometry_log = config["private_tap_transmit_ring_geometry"][name]
        link = dev if name == ns else "labtap"
        check(row["interface"] == geometry and geometry_log["new"] == 4096 and geometry_log["original"] == 1000
              and set(row["offloads"][link]) == keys and set(row["offloads"][link].values()) == {"on"}, f"TAP geometry/offloads differ in {name}")
    workers = config["workers_with_delivery_ownership"]
    ownership = [(w["cpu"], w["role"], w["sources"], w["destinations"]) for w in workers]
    expected = [(14, "server-egress", [0], []), (2, "client-ingress", [1, 4], [1]),
                (5, "client-ingress", [2], [2, 4]), (8, "client-ingress", [3], [3])]
    handoffs = [(h["source"], h["destination"], h["producer_cpu"], h["consumer_cpu"])
                for h in config["verified_handoffs"]]
    check(sorted(ownership) == sorted(expected) and [w["tid"] for w in workers] ==
          [w["tid"] for w in config["verified_workers"]] and
          sorted(handoffs) == [(0, 1, 14, 2), (0, 2, 14, 5), (0, 3, 14, 8), (0, 4, 14, 5)],
          "the original single-guest-TX reader and external lane ownership were not preserved")
    pf = config["preflight_forwarding"]
    check(pf["gso"] > 0 and pf["partial_checksum"] > 0 and not pf["bad_frame"] and not pf["queue_overflow"],
          "the checksum/GSO wire preflight did not pass cleanly")
    proofs = config["tcp_payload_proofs"]
    pairs = [topo[name].get("socket_probe_cpu_and_napi_id") for name in clients]
    check(len(proofs) == len(pairs) == 4 and all(pair == [[cpu, 0]]*4 for pair in pairs), "fresh four-lane TCP socket CPU/ID readbacks are absent or wrong")
    check(len(proofs) == 4 and all(
        p["service"]["cpus"] == [cpu] and p["service"]["napi_ids"] == [0] and p["service"]["count"] == 32 and
        0 < p["service"]["mss"] <= 1460 and 0 < p["client"]["mss"] <= 1460 and
        (p["client"]["count"], p["client"]["bytes"], p["client"]["sha256"]) == (32, 86296, EXPECTED_DIGEST)
        for p in proofs), "four-lane real TCP payload, observed receive CPU or checksum evidence differs")
    facts["verified_tcp_probes"] = {"per_lane_byte_checks": 32, "per_lane_short_socket_checks": 4, "lanes": len(proofs), "guest_cpu":cpu}
    start, hold, end = (timing[k] for k in ("stock_start_unix", "hold_starts_unix", "cutoff_unix"))
    facts["private_queue_reads"] = {}
    facts["service_cpu_receive_backlog"] = {}
    for stage, lo, hi in (("ramp", start, hold), ("hold", hold, end)):
        netrows = [r for r in network if lo <= r["unix"] < hi]
        facts["private_queue_reads"][stage] = len(netrows)
        check(len(netrows) >= 2 and all(queue_ok(r.get("private_service_receive"), cpu, ref) for r in netrows),
              f"private RPS queue readback coverage/identity is incomplete during {stage}")
        cpurows = [r["softnet"][str(cpu)] for r in measured if lo <= r["unix"] < hi and str(cpu) in r.get("softnet", {})]
        check(len(cpurows) >= 2, f"missing service CPU receive backlog counters during {stage}")
        if cpurows:
            facts["service_cpu_receive_backlog"][stage] = {k:cpurows[-1][k] - cpurows[0][k] for k in ("processed","dropped","time_squeeze")}
    active = [r for r in measured if start <= r["unix"] < end]
    if active and any(r.get("napi_threads") for r in active):
        errors.append("a private NAPI task was monitored by this nominally ordinary model")
    if active and all(str(cpu) in r.get("softnet", {}) for r in active):
        delta = active[-1]["softnet"][str(cpu)]["dropped"] - active[0]["softnet"][str(cpu)]["dropped"]
        if delta > 0:
            target.append(f"service CPU receive backlog dropped {delta} packets between scored samples; per-CPU counter")
        elif delta < 0:
            errors.append("service CPU backlog counter reset")
    else:
        errors.append("service CPU receive backlog telemetry is incomplete")
    return dict(facts=facts, infrastructure_flags=errors, target_network_flags=target)


def apply(base, result):
    base["softirq_rps_assessment"] = result
    base["infrastructure_flags"] += result["infrastructure_flags"]
    base["target_network_flags"] += result["target_network_flags"]
    if result["infrastructure_flags"]:
        base["infrastructure_ok"] = base["scored_healthy_environment"] = False
    if result["infrastructure_flags"] or result["target_network_flags"]:
        base["qualified"] = base["eligible"] = False
    return base


def assess(out):
    base = assess_common(out)
    manifest_file = ROOT / "manifest.json"
    try:
        manifest = read_json(manifest_file)
        if any(sha(ROOT / name) != digest for name,digest in manifest["files"].items()):
            raise ValueError("frozen source hashes differ during assessment")
        result = evidence(read_json(out / "shape.json"), read_json(out / "timing.json"),
                          rows(out / "network-samples.jsonl"), read_json(out / "external-at-client-exit.json"),
                          rows(out / "telemetry.jsonl"), manifest, sha(manifest_file))
    except (OSError, KeyError, IndexError, TypeError, ValueError, AttributeError) as exc:
        result = dict(facts={}, infrastructure_flags=[f"incomplete private RPS evidence: {exc}"], target_network_flags=[])
    apply(base, result)
    (out / "summary.json").write_text(json.dumps(base, indent=2, sort_keys=True) + "\n")
    return base


if __name__ == "__main__":
    scored = assess(Path(sys.argv[1]))
    print(json.dumps({k:scored[k] for k in ("tag","stock_pass","qualified","infrastructure_flags","target_network_flags")}))
