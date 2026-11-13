"""Read CPU delivery from complete or in-progress split TAP trials without changing their scores."""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "router"))
from assess import read_json, split_worker_schedule


def report(directory):
    shape = read_json(directory / "shape.json")
    clock = read_json(directory / "timing.json")
    config = shape["network"][shape["namespaces"][1]].get("polling_relay", {})
    with (directory / "network-samples.jsonl").open() as stream:
        samples = [json.loads(line) for line in stream if line.endswith("\n")]
    rows, reasons, complete = split_worker_schedule(
        samples, config.get("verified_workers", []), clock["stock_start_unix"],
        clock["hold_starts_unix"], clock["cutoff_unix"],
    )
    final = (directory / "summary.json").is_file()
    return {
        "trial": directory.name,
        "profile": shape["topology"],
        "score_file_present": final,
        "scheduler_samples_complete_in_observed_intervals": complete,
        "workers": rows,
        "observed_contention": reasons,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+")
    for path in parser.parse_args().runs:
        print(json.dumps(report(path)))
