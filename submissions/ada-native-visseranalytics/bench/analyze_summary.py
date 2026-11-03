#!/usr/bin/env python3
"""Summarize k6 shards without inventing a merged latency percentile."""
import json
from pathlib import Path
import sys

directory = Path(sys.argv[1])
plan = json.loads((directory / "plan.json").read_text())
shards = []
for index in range(plan["shards"]):
    child = directory / f"shard-{index}"
    metrics = json.loads((child / "summary.json").read_text())["metrics"]
    container = json.loads((child / "loadgen.json").read_text())[0]
    row = dict(index=index, observed_max_vus=metrics["vus"]["max"],
               requests=metrics["http_reqs"]["count"],
               failed_requests=metrics["http_req_failed"]["passes"],
               failure_rate=metrics["http_req_failed"]["value"],
               p95_ms=metrics["http_req_duration"]["p(95)"],
               p99_ms=metrics["http_req_duration"]["p(99)"],
               exit_code=int((child / "exit.txt").read_text()),
               oom=container["State"]["OOMKilled"])
    row["pass"] = (row["observed_max_vus"] == plan["vus_per_shard"] and
                   row["exit_code"] == 0 and not row["oom"] and
                   row["p95_ms"] < 500 and row["p99_ms"] < 1000 and
                   row["failure_rate"] < .01)
    shards.append(row)
total = sum(s["requests"] for s in shards)
failed = sum(s["failed_requests"] for s in shards)
result = dict(target_vus=plan["target_vus"], duration=plan["duration"],
              pass_all_shards=all(s["pass"] for s in shards), shards=shards,
              whole_run=dict(requests=total, failed_requests=failed,
                             failure_rate=failed / total),
              notes="Local diagnostic. Each shard enforces the unchanged k6 thresholds. "
                    "Percentiles include ramps; no merged percentile or hold-only metrics inferred.")
serialized = json.dumps(result, indent=2) + "\n"
(directory / "summary-analysis.json").write_text(serialized)
print(serialized)
