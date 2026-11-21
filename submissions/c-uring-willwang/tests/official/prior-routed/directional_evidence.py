"""Reconstruct the matched C280 two-core router sensitivity from its raw archives."""

import argparse
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from collect import collect
from replay import read, sha


def receipt(path):
    path = path.resolve()
    scored = collect(path)
    shape = read(path / "shape.json")
    raw = path / "raw"
    start = read(raw / "runtime.json")["start_unix_ms"] / 1000
    cutoff = start + shape["ramp_s"] + shape["hold_s"]
    samples = [json.loads(x) for x in (raw / "samples.jsonl").read_text().splitlines()]
    window = [x for x in samples if x["wall"] <= cutoff]
    if window[0]["scheduled_s"] != 0 or window[-1]["scheduled_s"] != shape["ramp_s"] + shape["hold_s"] - 0.25:
        raise ValueError("both private network counter boundaries are required")
    first, last = window[0], window[-1]

    def devices(sample):
        values = {}
        for lines in sample["private_devices"].values():
            for line in lines:
                device, _, counters = line.partition(":")
                if device.strip() != "lo":
                    values[device.strip()] = int(counters.split()[3])
        if set(values) != {"rserver", "rclient", "tserver", "tclient"}:
            raise ValueError("incomplete private interface RX counters")
        return values

    def softnet(sample):
        return [int(x.split()[1], 16) for x in sample["host_softnet_stat_by_cpu"]]

    old, end = devices(first), devices(last)
    drops = {k: end[k] - old[k] for k in end}
    per_cpu = [b - a for a, b in zip(softnet(first), softnet(last), strict=True)]
    if len(per_cpu) != 16 or any(v < 0 for v in [*drops.values(), *per_cpu]):
        raise ValueError("unexpected CPU topology or nonmonotonic counters")
    onset = None
    for prev, cur in zip(window, window[1:]):
        if devices(cur) != old:
            onset = {
                "between_s": [prev["scheduled_s"], cur["scheduled_s"]],
                "at_established": cur["tcp"]["CurrEstab"],
            }
            break
    trace = read(path / "synchronized-connections.json")
    planned = trace["expected"]
    aligned = trace["hold_samples"]
    at_least_one_full_window = ((shape["ramp_s"] + 9) // 5) * 5
    complete = [r for r in aligned if r["t"] >= at_least_one_full_window]
    plan = read(raw / "plan.json")
    complete_expected = list(range(at_least_one_full_window, plan["ramp_s"] + plan["hold_s"], 5))
    if [r["t"] for r in complete] != complete_expected or not complete:
        raise ValueError("missing full five-second generator samples")
    files = ["shape.json", "placements.json", "charged-descendants.jsonl", "component-test-lock.json",
             "serial-lock.json", "scored-assessment.json", "raw/plan.json", "raw/runtime.json",
             "raw/samples.jsonl", "raw/score.json", *[f"raw/shard{i}.json" for i in range(4)]]
    proof_path = path / "directional-rps.json"
    device_masks = None
    if proof_path.is_file():
        files.append(proof_path.name)
        device_masks = {n: v["queues"] for n, v in read(proof_path)["actual_queues"].items()}
    stock = read(raw / "score.json")
    router_drops = drops["rserver"] + drops["rclient"]
    return {
        "tag": scored["tag"], "topology": scored["topology"], "input_profile": scored["input_profile"],
        "users": scored["users"], "ramp_s": scored["ramp_s"], "hold_s": scored["hold_s"],
        "stock_pass": scored["stock_pass"], "qualified": scored["qualified"],
        "p95_ms": scored["p95_ms"], "p99_ms": scored["p99_ms"], "requests": stock["reqs"],
        "requests_per_s": scored["req_per_s"], "failures": scored["fails"], "dials": scored["dials"],
        "scheduled_shard_dials": trace["dials"], "scored_to_s": last["scheduled_s"],
        "actual_directional_queue_masks": device_masks,
        "all_snapshots_full": [sum(r["shards"] == planned for r in aligned), len(aligned)],
        "complete_5s_windows_full": [sum(r["shards"] == planned for r in complete), len(complete)],
        "infrastructure_flags": scored["infrastructure_flags"], "first_private_rx_drop": onset,
        "interface_rx_drops": drops, "softnet_backlog_drops_by_cpu": per_cpu,
        "external_router_rx_drops": router_drops, "external_rx_drops_per_1000_requests": 1000 * router_drops / stock["reqs"],
        "target_retransmissions": last["tcp"]["RetransSegs"] - first["tcp"]["RetransSegs"],
        "mload_sha256": shape["mload_sha256"], "server_sha256": shape["server_sha256"],
        "seed_sha256": shape["seed_sha256"], "tokens_sha256": shape["tokens_sha256"],
        "original_seven": shape["original_seven"], "signed_hash_by_role": scored["private_hash_by_role"],
        "input_artifact_sha256": {n: sha(path / n) for n in files},
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--smoke", type=Path, required=True)
    p.add_argument("--original", type=Path, required=True)
    p.add_argument("--directional", type=Path, required=True)
    a = p.parse_args()
    smoke, original, altered = [receipt(d) for d in (a.smoke, a.original, a.directional)]
    shared = ("input_profile", "mload_sha256", "server_sha256", "seed_sha256", "tokens_sha256",
              "original_seven", "signed_hash_by_role")
    if not all(smoke[k] == original[k] == altered[k] for k in shared):
        raise ValueError("unmatched original and directional inputs")
    if (smoke["users"], smoke["ramp_s"], smoke["hold_s"], smoke["stock_pass"], smoke["qualified"]) != (128, 60, 45, True, True):
        raise ValueError("no qualified 128-user smoke")
    if any((d["users"], d["ramp_s"], d["hold_s"]) != (280000, 84, 300) for d in (original, altered)):
        raise ValueError("paired controls require 280000 users and matching native clocks")
    if (original["topology"] != "historical-single-middle-router-paced" or
        smoke["topology"] != altered["topology"] or not altered["actual_directional_queue_masks"]):
        raise ValueError("unexpected scored pair geometry")
    output = {"smoke": smoke, "original": original, "directional": altered}
    output["directional_over_original"] = {
        field: altered[field] / original[field] if original[field] else None
        for field in ("external_router_rx_drops", "external_rx_drops_per_1000_requests", "requests_per_s", "p99_ms")
    }
    print(json.dumps(output, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
