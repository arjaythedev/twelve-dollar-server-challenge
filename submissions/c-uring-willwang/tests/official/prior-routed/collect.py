#!/usr/bin/env python3
"""Validate one archived replay in place, without creating sockets, DB copies or benchmark runs."""
import argparse
import json
from pathlib import Path
import sys
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from observe import occupancy, wire_evidence
from assess_window import assess
from replay import PROFILE, STOCK, HISTORICAL_INPUTS, read, sources
import archived_score
import directional


def check(condition, message):
    if not condition:
        raise ValueError(message)


def collect(out):
    out = out.resolve()
    shape = read(out / "shape.json")
    result = read(out / "summary.json")
    raw = (out / "raw").resolve()
    check(raw == out / "work/runs" / out.name, "raw output redirected to another trial")
    split_directions = out.name.startswith("priorrps-")
    expected_topology = directional.PROFILE if split_directions else PROFILE
    check(
        shape["topology"] == expected_topology and out.name == shape["tag"] == result["tag"],
        "trial identity",
    )
    if split_directions:
        check(shape.get("input_profile") == "common", "unexpected directional input profile")
        directional.verify(out)
    check(shape["original_seven"] == sources(), "historical source mismatch")
    plan, original, merged = [
        read(raw / f) for f in ("plan.json", "summary.json", "score.json")
    ]
    check(
        plan["generator_sha256"] == shape["mload_sha256"] == STOCK,
        "mload does not match the archived binary",
    )
    if shape.get("input_profile") == "historical-exact" or out.name.startswith(
        "priorroute-exact-"
    ):
        check(
            shape.get("input_profile") == "historical-exact"
            and out.name.startswith("priorroute-exact-")
            and {"feed.db": shape["seed_sha256"], "tokens.json": shape["tokens_sha256"]}
            == HISTORICAL_INPUTS,
            "historical exact-input trial uses another image or token set",
        )
    for recorded, canonical in [
        ("server_sha256", "server_sha256"),
        ("seed_sha256", "seed_sha256"),
        ("tokens_sha256", "tokens_sha256"),
        ("hold_s", "hold_s"),
        ("ramp_s", "ramp_s"),
        ("users", "users"),
    ]:
        check(plan[recorded] == shape[canonical], f"shape changed: {canonical}")
    suite = archived_score
    suite.WORK = out / "work"
    args = SimpleNamespace(**shape)
    archived = suite.inspect(
        "router-paced",
        Path(shape["server"]).name,
        "replay",
        args.users,
        args.hold_s,
        args.tag,
    )
    observed = occupancy(raw, args)
    check(
        archived == read(out / "historical-assessment.json"),
        "historical eligibility changed",
    )
    check(
        observed == read(out / "synchronized-connections.json"),
        "four-generator simultaneous records changed",
    )
    check(
        result["stock_pass"]
        == original["pass"]
        == merged["pass"]
        == archived["stock_pass"],
        "stock result changed",
    )
    for k in ["reqs", "fails", "dials", "p95_ms", "p99_ms", "req_per_s", "duration_s"]:
        check(result[k] == original[k] == merged[k], f"preserved score changed: {k}")
    reports = [read(raw / f"shard{i}.json") for i in range(4)]
    samples = [
        json.loads(line) for line in (raw / "samples.jsonl").read_text().splitlines()
    ]
    wire = wire_evidence(samples, plan, reports)
    check(
        wire == read(out / "scored-wire-cpu.json"),
        "private scored drop, CPU or traffic deltas changed",
    )
    for k in ["reqs", "fails", "dials", "users"]:
        check(
            sum(s[k] for s in reports) == merged[k], f"shards disagree with merge: {k}"
        )
    for k in ("hist", "connect_hist"):
        # The stock exporter serializes fixed ordered arrays. Rounding only affects
        # the presented scalar quantiles; each bin sum remains an exact integer.
        check(
            [sum(g) for g in zip(*(s[k] for s in reports), strict=True)] == merged[k],
            f"histogram changed: {k}",
        )
    cpu9 = read(out / "component-test-lock.json")
    serial = read(out / "serial-lock.json")
    check(
        cpu9["lock_file"] == Path(shape["cpu9_lock"]).name
        and serial["lock_file"] == Path(shape.get("serial_lock", "active.lock")).name,
        "lock mismatch",
    )
    for lock in (cpu9, serial):
        check(
            lock["acquired_unix"] < lock["finished_unix"] and lock["runner_exit"] == 0,
            "protected launch or teardown failed",
        )
    runtime = read(raw / "runtime.json")
    start = runtime["start_unix_ms"] / 1000
    check(
        all(
            l["acquired_unix"] <= start
            and l["finished_unix"] >= start + args.ramp_s + args.hold_s
            for l in (cpu9, serial)
        ),
        "lock did not cover the complete score",
    )
    descendants = [
        json.loads(row)
        for row in (out / "charged-descendants.jsonl").read_text().splitlines()
    ]
    check(
        len(descendants) == result["service_scope_samples"]
        and all(
            row["resources"]["memory.max"] == str(1750 * 1024**2)
            and row["resources"]["memory.swap.max"] == "0"
            for row in descendants
        ),
        "scope evidence changed",
    )
    check(
        result["historical_eligible"] == archived["eligible"]
        and result["full_presence"] == observed["simultaneous_all_shards"],
        "qualification fields changed",
    )
    robust = assess(out, plan, original, merged, observed, wire, cpu9["runner_exit"])
    if (out / "scored-assessment.json").is_file():
        check(
            robust == read(out / "scored-assessment.json"),
            "raw strict qualification audit changed",
        )
    check(
        robust["qualified"] == result["qualified"]
        and robust["flags"] == result["infrastructure_flags"]
        and robust["scored_oom"] == result["app_oom"],
        "raw scope, dials, qdisc or timed app lifetime contradict the result",
    )
    topology = read(out / "placements.json")
    check(
        len(topology["lanes"]) == 4
        and all(f"incoming_cpu=10:24," in row["server"] for row in topology["lanes"]),
        "private shard receive placement missing",
    )
    check(
        result["private_hash_by_role"]
        == {k: v["ipv4/tcp_ehash_entries"] for k, v in topology["namespaces"].items()},
        "signed hash readbacks changed",
    )
    fields = {
        k: result[k]
        for k in (
            "tag",
            "users",
            "stock_pass",
            "historical_eligible",
            "qualified",
            "full_presence",
            "infrastructure_flags",
            "p95_ms",
            "p99_ms",
            "req_per_s",
            "fails",
            "dials",
            "app_oom",
            "private_hash_by_role",
            "hold_server_cpu",
            "whole_run_server_cpu",
        )
    }
    return {
        **fields,
        "server_sha256": shape["server_sha256"],
        "seed_sha256": shape["seed_sha256"],
        "tokens_sha256": shape["tokens_sha256"],
        "ramp_s": plan["ramp_s"],
        "hold_s": plan["hold_s"],
        "topology": shape["topology"],
        "input_profile": shape.get("input_profile", "common"),
    }


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "trials", nargs="+", type=Path, help="one or more completed replay directories"
    )
    p.add_argument(
        "--matched-cohort",
        action="store_true",
        help="require the same geometry, data and schedule, and a distinct binary for every supplied trial",
    )
    args = p.parse_args()
    rows = [collect(trial) for trial in args.trials]
    if args.matched_cohort:
        dimensions = (
            "users",
            "ramp_s",
            "hold_s",
            "topology",
            "input_profile",
            "seed_sha256",
            "tokens_sha256",
        )
        check(
            len(set(tuple(row[k] for k in dimensions) for row in rows)) == 1,
            "cohort mixes targets, durations, input data or topology",
        )
        check(
            len({row["server_sha256"] for row in rows}) == len(rows) and len(rows) > 1,
            "supply at least two distinct server implementations",
        )
        check(
            len(
                {
                    json.dumps(row["private_hash_by_role"], sort_keys=True)
                    for row in rows
                }
            )
            == 1,
            "signed TCP-table geometry differs across the cohort",
        )
    for row in rows:
        print(json.dumps(row, sort_keys=True))
