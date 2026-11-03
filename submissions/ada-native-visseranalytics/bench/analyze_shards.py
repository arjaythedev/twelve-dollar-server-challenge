#!/usr/bin/env python3
"""Merge raw samples, not shard percentiles, and report overlapping full-VU time."""
import datetime as dt
import gzip
import json
import math
from pathlib import Path
import re
import sys


def stamp(value):
    value = re.sub(r"\.(\d+)", lambda m: "." + m[1][:6].ljust(6, "0"), value)
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def quantile(sorted_values, q):
    index = q * (len(sorted_values) - 1)
    lo, hi = math.floor(index), math.ceil(index)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (index - lo)


def metrics(durations, failures, seconds=None):
    values = sorted(v for _, v in durations)
    failed = sum(v for _, v in failures)
    assert values and len(values) == len(failures)
    p95, p99 = quantile(values, .95), quantile(values, .99)
    rate = failed / len(values)
    return dict(requests=len(values), failed_requests=failed, failure_rate=rate,
                p95_ms=p95, p99_ms=p99, max_ms=values[-1],
                rps=len(values) / seconds if seconds else None,
                thresholds_pass=p95 < 500 and p99 < 1000 and rate < .01)


def memory_mib(value):
    match = re.match(r"([\d.]+)([A-Za-z]+)", value.split(" / ")[0])
    return float(match[1]) * {"B": 1 / 1024**2, "KiB": 1 / 1024, "MiB": 1, "GiB": 1024}[match[2]]


def resources(rows):
    cpus = [r[1] for r in rows]
    mem = [r[2] for r in rows]
    return dict(samples=len(rows), cpu_mean_percent=sum(cpus) / len(cpus) if cpus else None,
                cpu_max_percent=max(cpus, default=None), memory_max_mib=max(mem, default=None))


def main():
    directory = Path(sys.argv[1])
    plan = json.loads((directory / "plan.json").read_text())
    durations, failures, starts, ends, shards = [], [], [], [], []
    for i in range(plan["shards"]):
        child = directory / f"shard-{i}"
        summary = json.loads((child / "summary.json").read_text())["metrics"]
        points = {"http_req_duration": [], "http_req_failed": [], "vus": []}
        with gzip.open(child / "samples.json.gz", "rt") as stream:
            for line in stream:
                if not any('"' + key + '"' in line for key in points):
                    continue
                item = json.loads(line)
                name = item.get("metric")
                if item.get("type") == "Point" and name in points:
                    points[name].append((stamp(item["data"]["time"]), item["data"]["value"]))
        count = summary["http_reqs"]["count"]
        assert len(points["http_req_duration"]) == len(points["http_req_failed"]) == count
        assert sum(v for _, v in points["http_req_failed"]) == summary["http_req_failed"]["passes"]
        full = [t for t, v in points["vus"] if v == plan["vus_per_shard"]]
        assert full, f"Shard {i} did not reach its assigned VUs"
        start = min(full)
        end = min(t for t, v in points["vus"] if t > start and v < plan["vus_per_shard"])
        starts.append(start)
        ends.append(end)
        durations.extend(points["http_req_duration"])
        failures.extend(points["http_req_failed"])
        container = json.loads((child / "loadgen.json").read_text())[0]
        shards.append(dict(index=i, observed_max_vus=max(v for _, v in points["vus"]),
                           exit_code=int((child / "exit.txt").read_text()),
                           oom=container["State"]["OOMKilled"],
                           cpus=container["HostConfig"]["NanoCpus"] / 1e9,
                           memory_limit_bytes=container["HostConfig"]["Memory"],
                           whole_run=metrics(points["http_req_duration"], points["http_req_failed"])))
    start, end = max(starts), min(ends)
    assert end > start, "No overlapping full-VU hold"
    hold = metrics([(t, v) for t, v in durations if start <= t < end],
                   [(t, v) for t, v in failures if start <= t < end], end - start)
    hold.update(start_utc=dt.datetime.fromtimestamp(start, dt.timezone.utc).isoformat(),
                end_utc=dt.datetime.fromtimestamp(end, dt.timezone.utc).isoformat(), seconds=end - start)
    groups, aggregates = {}, {}
    for line in (directory / "stats.jsonl").read_text().splitlines():
        row = json.loads(line)
        at = stamp(row["sampled_at"])
        stats = row["stats"]
        name = "server" if stats["Name"].endswith("-server-1") else "shard-" + stats["Name"].rsplit("-", 1)[1]
        values = (at, float(stats["CPUPerc"].rstrip("%")), memory_mib(stats["MemUsage"]))
        groups.setdefault(name, []).append(values)
        if name != "server":
            batch = aggregates.setdefault(at, [0, 0, 0])
            batch[0] += values[1]
            batch[1] += values[2]
            batch[2] += 1
    groups["all_generators"] = [(t, cpu, mem) for t, (cpu, mem, n) in aggregates.items() if n == plan["shards"]]
    usage = {name: dict(whole_run=resources(rows),
                        estimated_hold=resources([r for r in rows if start <= r[0] < end]))
             for name, rows in groups.items()}
    server = json.loads((directory / "server-after.json").read_text())[0]
    report = dict(target_vus=plan["target_vus"], shards=shards,
                  whole_run=metrics(durations, failures), estimated_common_hold=hold, resources=usage,
                  server=dict(state=server["State"], image=server["Image"],
                              cpus=server["HostConfig"]["NanoCpus"] / 1e9,
                              memory_limit_bytes=server["HostConfig"]["Memory"]),
                  pass_all_shards=all(s["exit_code"] == 0 and not s["oom"] for s in shards),
                  notes="Local emulated screen; hold boundaries estimated from VU samples. Percentiles merged from raw samples. Docker stats are samples, not cgroup peaks. CPU 100% = one core.")
    write = json.dumps(report, indent=2) + "\n"
    (directory / "analysis.json").write_text(write)
    print(write)


if __name__ == "__main__":
    main()
