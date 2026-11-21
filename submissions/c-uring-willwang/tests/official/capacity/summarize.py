"""Extract comparable results without discarding failed or unqualified trials."""

import argparse
import hashlib
import json
import math
from pathlib import Path
from statistics import median
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "router"))
from assess import read_json, rows, split_worker_schedule


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def stock_participation(shards, shape):
    """Check the independently assigned population on each stock generator."""
    count, extra = divmod(shape["users"], len(shards))
    assigned = [count + (index < extra) for index in range(len(shards))]
    first = 5 * math.ceil((shape["ramp_s"] + 5) / 5)
    expected = set(range(first, shape["ramp_s"] + shape["hold_s"], 5))
    full = expected.copy()
    for required, shard in zip(assigned, shards, strict=True):
        observations = {}
        for tick in shard["timeline"]:
            t = 5 * round(tick["t"] / 5)
            if t in expected and abs(tick["t"] - t) <= 1:
                observations.setdefault(t, []).append(tick)
        full.intersection_update(t for t, records in observations.items()
                                 if len(records) == 1 and
                                 records[0]["users"] == records[0]["conns"] == required)
    return {
        "assigned": assigned,
        "reported_users": [shard["users"] for shard in shards],
        "actual_dials": [shard["dials"] for shard in shards],
        "expected_ticks": len(expected),
        "incomplete_or_ambiguous_ticks": sorted(expected - full),
    }


def request_histogram(directory):
    shard_files = sorted(directory.glob("shard-*.json"))
    if not shard_files:
        return None
    merged = read_json(directory / "merged.json")
    hist = merged["hist"]
    shards = [read_json(path) for path in shard_files]
    if hist != [sum(bucket) for bucket in zip(*(s["hist"] for s in shards), strict=True)]:
        raise ValueError(f"{directory.name}: stock merge differs from the shard counts")
    observations = sum(hist)
    for probability, field in ((0.95, "p95_ms"), (0.99, "p99_ms")):
        target = math.ceil(observations * probability)
        cumulative = 0
        latency = 0.0
        if observations:
            for index, count in enumerate(hist):
                cumulative += count
                if cumulative >= target:
                    latency = 1.01 ** (index + 0.5) / 1000
                    break
        if not math.isclose(latency, merged[field], rel_tol=1e-10):
            raise ValueError(f"{directory.name}: {field} differs from the stock histogram")
    occupied = [i for i, count in enumerate(hist) if count]
    first = occupied[0] if occupied else 0
    stop = occupied[-1] + 1 if occupied else 0
    result = {
        "trial": directory.name,
        "shard_count": len(shard_files),
        "histogram_observations": observations,
        "endpoint_observations": merged["reqs"],
        "bucket_count": len(hist),
        "first_bucket": first,
        "counts": hist[first:stop],
        "errors": merged.get("errors", {}),
        "endpoints": [
            {key: endpoint[key] for key in ("name", "ok", "fail", "p95_ms", "p99_ms")}
            for endpoint in merged["endpoints"]
        ],
    }
    shape = read_json(directory / "shape.json")
    if {"users", "ramp_s", "hold_s"} <= shape.keys():
        participation = stock_participation(shards, shape)
        if sum(participation["actual_dials"]) != merged["dials"]:
            raise ValueError(f"{directory.name}: merged TCP dials differ from the shard counts")
        result["stock_participation"] = participation
    return result


def hold_service_cpu(directory, timing):
    path = directory / "samples.jsonl"
    if not path.exists() or not timing or not {"hold_starts_unix", "cutoff_unix"} <= timing.keys():
        return None
    start, end = timing["hold_starts_unix"], timing["cutoff_unix"]
    rows = []
    with path.open() as source:
        for line in source:
            sample = json.loads(line)
            if start <= sample["unix"] < end and "cpu.stat" in sample.get("app", {}):
                rows.append(sample)
    if len(rows) < 2:
        return None
    first, last = rows[0], rows[-1]
    elapsed = last["unix"] - first["unix"]
    old, new = first["app"]["cpu.stat"], last["app"]["cpu.stat"]
    return {
        "sampled_seconds": round(elapsed, 2),
        "total_pct": round((new["usage_usec"] - old["usage_usec"]) / (10000 * elapsed), 2),
        "user_pct": round((new["user_usec"] - old["user_usec"]) / (10000 * elapsed), 2),
        "system_pct": round((new["system_usec"] - old["system_usec"]) / (10000 * elapsed), 2),
    }


def host_observations(directory, result):
    checks = result.get("host_checks", {})
    drops = checks.get("tap_drop_attribution", {})
    hands = checks.get("reply_handoff_intervals", {}).get("hold", {})
    workers = checks.get("polling_worker_intervals", {}).get("hold", {})
    cpu = checks.get("cpu", {}).get("hold", {}).get("app", {})
    observations = {
        "service_cgroup_cpu": hold_service_cpu(directory, checks.get("timing")),
        "target_cpu_busy_pct": cpu.get("mean_busy_pct"),
        "target_cpu_steal_pct": cpu.get("mean_steal_pct"),
        "server_kernel_min_established": checks.get("server_kernel_curr_estab_hold_min"),
        "server_kernel_max_established": checks.get("server_kernel_curr_estab_hold_max"),
        "target_rx": drops.get("target_receive_interface"),
        "target_tx": drops.get("target_transmit_interface"),
        "relay_worker_min_idle_wall_pct": min(
            (w["minimum_idle_wall_time_pct"] for w in workers.values() if w.get("minimum_idle_wall_time_pct") is not None),
            default=None,
        ),
        "reply_queue_peak_per_lane": [h["cumulative_peak_at_stage_end"] for _, h in sorted(hands.items())],
        "reply_queue_full_per_lane": [h["full_polls"] for _, h in sorted(hands.items())],
    }
    post = directory / "teardown-memory.json"
    if post.exists():
        data = read_json(post)
        timing = read_json(directory / "timing.json")
        if data["cutoff_unix"] != timing["cutoff_unix"] or data["first"]["after_cutoff_s"] < 0:
            raise ValueError(f"{directory.name}: teardown samples have a different cutoff")
        observations["memory_after_stock_cutoff"] = {
            "first_sample_s": data["first"]["after_cutoff_s"],
            "last_sample_s": data["last"]["after_cutoff_s"],
            "samples": len(data["observations"]),
            "first_cumulative_peak_bytes": data["first"]["memory.peak"],
            "maximum_cumulative_peak_bytes": data["max_cgroup_peak_bytes"],
            "last_memory_events": data["last"]["memory.events"],
        }
    if result.get("passive_tcp_memory_status"):
        data = read_json(directory / "passive-memory-result.json")
        observations["passive_tcp_memory"] = {
            key: data.get(key)
            for key in (
                "scored_records", "scored_peak_live_ipv4", "scored_min_parent_limit_bytes",
                "scored_peak_current_plus_modeled_bytes", "oom_kill_at_cutoff", "fixture_flags",
                "max_before_cutoff_events", "p99_sample_gap_ms", "max_sample_gap_ms",
                "p99_write_ms", "manager_process_cpu_s",
            )
        }
    if result.get("softirq_rps_assessment"):
        observations["service_receive_steering"] = result["softirq_rps_assessment"]
    lock = directory / "component-test-lock.json"
    if lock.exists() and (directory / "timing.json").exists():
        record = read_json(lock)
        timing = read_json(directory / "timing.json")
        observations["component_test_lock_covered_score"] = (
            record["acquired_unix"] <= timing["stock_start_unix"]
            and record["finished_unix"] >= timing["cutoff_unix"]
        )
    trace = directory / "telemetry.jsonl"
    timing = checks.get("timing") or {}
    if trace.exists() and {"hold_starts_unix", "cutoff_unix"} <= timing.keys():
        counts = []
        with trace.open() as source:
            for line in source:
                row = json.loads(line)
                if not timing["hold_starts_unix"] + 5 <= row["unix"] < timing["cutoff_unix"]:
                    continue
                host = row.get("host_sockstat_tcp", {})
                local = row.get("Tcp", {})
                if "alloc" in host and "CurrEstab" in local:
                    counts.append(host["alloc"] - 2 * local["CurrEstab"])
        if counts:
            observations["host_alloc_minus_twice_service_established"] = {
                "median": median(counts), "maximum": max(counts), "samples": len(counts)
            }
    return observations


def check_relay_schedule(directory, relay, result):
    """Apply the same scheduler check to preserved runs and current runs."""
    if not relay.get("scheduler_observation", {}).get("external_relay_workers"):
        return None
    timing = read_json(directory / "timing.json")
    points = [row for row in rows(directory / "network-samples.jsonl") if row.get("polling_relay")]
    delivery, flags, complete = split_worker_schedule(
        points, relay["verified_workers"], timing["stock_start_unix"],
        timing["hold_starts_unix"], timing["cutoff_unix"],
    )
    if not complete:
        result["scored_external_queue_evidence"] = False
        flags.append("dedicated relay scheduler samples do not cover the scored intervals")
    if flags:
        result["infrastructure_flags"] = list(dict.fromkeys([*result.get("infrastructure_flags", []), *flags]))
        for field in ("infrastructure_ok", "scored_healthy_environment", "eligible", "qualified"):
            result[field] = False
    return {
        "complete": complete,
        "maximum_runnable_wait_pct": {
            stage: max((r["runnable_wait_pct"] for r in workers.values()
                        if r["runnable_wait_pct"] is not None), default=None)
            for stage, workers in delivery.items()
        },
    }


def summarize(directory, binaries):
    shape = read_json(directory / "shape.json")
    result = read_json(directory / "summary.json")
    preflight_failed = result.get("preflight", {}).get("passed") is False
    if preflight_failed:
        if (directory / "merged.json").exists():
            raise ValueError(f"{directory.name}: failed preflight has a scored load")
    else:
        if not isinstance(result.get("stock_pass"), bool) or "scored_external_queue_evidence" not in result:
            raise ValueError(f"{directory.name}: final score and external network assessment are pending")
        if (shape.get("topology", "").endswith("-private-passive-tcp-memory")
                and not result.get("passive_tcp_memory_status")):
            raise ValueError(f"{directory.name}: private memory control assessment is pending")
        if shape.get("topology", "").endswith("-ordinary-rps") and not result.get("softirq_rps_assessment"):
            raise ValueError(f"{directory.name}: private receive steering assessment is pending")
        completed = {file.name for file in directory.glob("shard-*.json")}
        if completed != {f"shard-{i}.json" for i in range(4)} or not (directory / "merged.json").exists():
            raise ValueError(f"{directory.name}: the four stock shard outputs and their merge are required")
    digest = shape.get("server_sha256")
    fixture = {
        field: shape.get(field)
        for field in (
            "topology", "kernel", "app_cpu", "app_memory_bytes", "fd_limit",
            "router_cpus", "client_cpus", "manager_cpu", "target_aliases",
            "router_delay_us_each_direction", "server_ehash_requested",
            "seed_sha256", "tokens_sha256", "mload_sha256",
        )
    }
    budget_path = directory / "service-controls.json"
    controls = read_json(budget_path).get("actual") if budget_path.exists() else None
    if controls:
        fixture["service_cpu_max"] = controls["cpu.max"]
        fixture["service_cpu_max_burst"] = controls.get("cpu.max.burst")
    host = host_observations(directory, result)
    volume = directory / "database-volume.json"
    if volume.exists():
        disk = read_json(volume)
        fixture["database_filesystem"] = disk["filesystem"]
        if not preflight_failed:
            clock = read_json(directory / "timing.json")
            scored = [row for row in disk["samples"]
                      if clock["stock_start_unix"] <= row["unix"] < clock["cutoff_unix"]]
            if not scored:
                raise ValueError(f"{directory.name}: no filesystem samples during the score")
            if not any("feed.db" in row for row in scored):
                raise ValueError(f"{directory.name}: database file was never observed during the score")
            for row in scored:
                for name in ("feed.db", "feed.db-wal"):
                    if name in row and row[name]["device"] != disk["filesystem"]["maj:min"]:
                        raise ValueError(f"{directory.name}: {name} is on a different block device")
            wal_allocations = [row["feed.db-wal"]["allocated"] for row in scored if "feed.db-wal" in row]
            host["database_volume"] = {
                "sample_cpu": disk["sample_cpu"],
                "scored_samples": len(scored),
                "first_scored_sample_s": round(min(row["unix"] for row in scored) - clock["stock_start_unix"], 3),
                "last_scored_sample_s": round(max(row["unix"] for row in scored) - clock["stock_start_unix"], 3),
                "database_samples": sum("feed.db" in row for row in scored),
                "wal_samples": len(wal_allocations),
                "minimum_available_bytes": min(row["available_bytes"] for row in scored),
                "peak_sampled_wal_allocated_bytes": max(wal_allocations, default=None),
            }
    namespaces = shape.get("namespaces", [])
    setup_digests = None
    if namespaces:
        network = shape["network"]
        rx = network[namespaces[0]]
        cpu_fields = {item["field"]: item["data"] for item in shape.get("cpu_description", {}).get("lscpu", [])}
        fixture["app_cpu_model"] = cpu_fields.get("Model name:")
        fixture["server_ehash_actual"] = rx.get("ehash_entries")
        fixture["server_interface"] = rx.get("interface")
        fixture["server_napi_threads"] = sum(
            len(interface["napi"]) for interface in rx.get("threaded_napi", {}).values()
        )
        relay = network[namespaces[1]].get("polling_relay", {})
        if relay:
            if not preflight_failed:
                schedule = check_relay_schedule(directory, relay, result)
                if schedule is not None:
                    host["dedicated_relay_schedule"] = schedule
            setup_digests = relay.get("fixture_source_sha256_at_launch")
            fixture["relay_sha256"] = relay["binary_sha256"]
            fixture["relay_cpu"] = relay["cpu"]
            fixture["relay_cpus"] = relay.get("cpus", [relay["cpu"]])
            fixture["relay_architecture"] = relay.get("architecture")
            if "passive_tcp_memory" in relay:
                memory = relay["passive_tcp_memory"]
                fixture["passive_tcp_memory"] = {
                    key: memory[key] for key in (
                        "leaf_limit", "leaf_swap_max", "upper_envelope_bytes",
                        "missing_bytes_per_socket", "counter", "cadence_ms",
                    )
                }
    connections = result.get("simultaneous_connections", {})
    measures = {
        field: result.get(field)
        for field in (
            "users", "users_active", "reqs", "req_per_s", "fails", "err_pct",
            "p95_ms", "p99_ms", "dials", "app_peak_mib", "oom_kill_at_cutoff", "stock_pass",
            "stock_schedule_complete", "raw_eligible", "all_scheduled_users_present",
            "infrastructure_ok", "scored_healthy_environment", "wire_eligible", "eligible", "qualified",
            "scored_external_queue_evidence", "infrastructure_flags", "target_network_flags",
        )
    }
    if "passive_tcp_memory_status" in result:
        measures["passive_tcp_memory_status"] = result["passive_tcp_memory_status"]
    return {
        "trial": directory.name,
        "assessment": "preflight_failed" if preflight_failed else "scored",
        "entry": binaries.get(digest),
        "server_sha256": digest,
        "utc_started": shape.get("utc_started"),
        "generator_user_offsets": shape.get("user_offsets"),
        "setup_source_sha256_at_launch": setup_digests,
        "ramp_s": shape.get("ramp_s"),
        "hold_s": shape.get("hold_s"),
        "preflight_passed": shape.get("preflight", {}).get("passed"),
        "hold_full_connection_ticks": connections.get("full_ticks"),
        "hold_expected_connection_ticks": connections.get("expected_5s_ticks"),
        "hold_min_established": connections.get("min_established"),
        "host_observations": host,
        "stock_output_sha256": {
            path.name: sha256(path) for path in sorted(directory.glob("shard-*.json"))
        },
        **measures,
    }, fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trials", type=Path, nargs="+", help="finished run directories")
    parser.add_argument("--entries", type=Path, default=Path(__file__).with_name("entries.json"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--histograms", type=Path,
                        help="also export each stock request histogram as a compact JSON line")
    args = parser.parse_args()
    binaries = {item["local_server_sha256"]: item["entry"] for item in read_json(args.entries)}
    fixtures = []
    results = []
    histograms = []
    for directory in args.trials:
        record, fixture = summarize(directory, binaries)
        if fixture not in fixtures:
            fixtures.append(fixture)
        record["fixture"] = fixtures.index(fixture) + 1
        results.append(record)
        if args.histograms:
            histogram = request_histogram(directory)
            if histogram is not None:
                histograms.append(histogram)
    report = {"fixtures": fixtures, "trials": results}
    data = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(data)
    else:
        print(data, end="")
    if args.histograms:
        args.histograms.write_text("".join(
            json.dumps(row, separators=(",", ":"), allow_nan=False) + "\n" for row in histograms
        ))


if __name__ == "__main__":
    main()
