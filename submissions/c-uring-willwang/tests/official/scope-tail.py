"""Read a local scoring scope's memory counters around the Go clients' simultaneous shutdown."""

import argparse
import errno
import json
from pathlib import Path
import sys
import time

LEAD_S = 5
TAIL_S = 30
INTERVAL_S = 0.1


def metadata(run, timeout):
    deadline = time.monotonic() + timeout
    while True:
        try:
            # The runner writes these files directly. A reader can catch a partial write.
            return tuple(json.loads((run / name).read_text()) for name in ("plan.json", "runtime.json"))
        except (FileNotFoundError, json.JSONDecodeError) as exc:
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for complete plan.json and runtime.json in {run}"
                ) from exc
            time.sleep(0.1)


def counters(path):
    return {k: int(v) for k, v in (line.split() for line in path.read_text().splitlines())}


def observe(run, wait_s):
    plan, runtime = metadata(run, wait_s)
    scope = Path(runtime["scope"])
    memory_max = plan["server_memory_mib"] * 2**20
    cutoff = runtime["start_unix_ms"] / 1000 + plan["ramp_s"] + plan["hold_s"]
    cutoff_mono = time.monotonic() + (cutoff - time.time())
    rows = []
    pending = None
    limit_verified = False
    reason = "observation_window_ended"

    try:
        if int((scope / "memory.max").read_text()) != memory_max:
            raise ValueError(f"{scope}/memory.max differs from the run plan")
        limit_verified = True
        # Poll only the directory before the observation window. An aborted run can remove it early.
        while (remaining := cutoff_mono - LEAD_S - time.monotonic()) > 0:
            scope.stat()
            time.sleep(min(remaining, 1))
        while time.monotonic() <= cutoff_mono + TAIL_S:
            tick = time.monotonic()
            current = int((scope / "memory.current").read_text())
            peak = int((scope / "memory.peak").read_text())
            offset = time.time() - cutoff
            pending = {"offset_from_score_end_s": offset, "memory_current": current, "memory_peak": peak}
            pending["events"] = counters(scope / "memory.events")
            stat = counters(scope / "memory.stat")
            pending.update({key: stat[key] for key in ("sock", "kernel", "slab", "file", "anon")})
            rows.append(pending)
            pending = None
            time.sleep(max(0, tick + INTERVAL_S - time.monotonic()))
    except OSError as exc:
        # Kernfs can report ENODEV on an open attribute while its cgroup disappears.
        if exc.errno not in (errno.ENOENT, errno.ENODEV, errno.ESTALE):
            raise
        # The directory can briefly outlive its attributes during kernfs removal.
        if scope.is_dir():
            time.sleep(INTERVAL_S)
            if scope.is_dir():
                raise
        reason = "scope_disappeared"
        if pending is not None:
            pending["partial"] = True
            rows.append(pending)

    def largest(observations):
        return max(observations, key=lambda row: row["memory_peak"], default=None)

    return {
        "case": run.name,
        "scope": str(scope),
        "native_pid": runtime["pid"],
        "memory_max_bytes": memory_max,
        "memory_limit_verified": limit_verified,
        "scheduled_cutoff_unix_s": cutoff,
        "sample_interval_s": INTERVAL_S,
        "ended_after_scope_disappeared": reason == "scope_disappeared",
        "end_reason": reason,
        "largest_sampled_before_completion": largest(r for r in rows if r["offset_from_score_end_s"] < 0),
        "largest_sampled_after_completion": largest(r for r in rows if r["offset_from_score_end_s"] >= 0),
        "rows": rows,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path, help="the run.sh output directory for this fresh tag")
    parser.add_argument("--wait", type=int, default=60, help="seconds to wait for the runner's metadata")
    args = parser.parse_args()
    if args.wait < 0:
        parser.error("--wait must be nonnegative")
    try:
        report = observe(args.run, args.wait)
    except (OSError, ValueError, RuntimeError) as exc:
        sys.exit(str(exc))
    print(json.dumps(report, indent=2))
    if not report["rows"]:
        sys.exit(
            "the scoring scope was unavailable throughout the observation window; no samples collected"
        )
