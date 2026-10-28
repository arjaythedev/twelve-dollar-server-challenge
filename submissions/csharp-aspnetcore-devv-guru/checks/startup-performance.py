#!/usr/bin/env python3
"""Compare cold processes, alternating publish variants on fresh database copies.

Run on Linux inside the desired CPU/memory cgroup. OS filesystem caches remain
warm; this measures process startup, not a cold machine or steady-state capacity.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import socket
import statistics
import subprocess
import tempfile
import time

from integration import token


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", action="append", required=True, help="label=/absolute/publish/directory")
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 3:
        parser.error("Use at least three samples per variant")
    variants = {}
    for value in args.variant:
        label, directory = value.split("=", 1)
        if not re.fullmatch(r"[a-z0-9-]+", label) or label in variants:
            parser.error("Variant labels must be unique lowercase names")
        executable = Path(directory).resolve() / "FeedApi"
        if not executable.is_file():
            parser.error(f"Missing executable: {executable}")
        variants[label] = executable
    args.output.parent.mkdir(parents=True, exist_ok=True)
    logs = args.output.parent / (args.output.stem + "-logs")
    logs.mkdir(exist_ok=True)
    cgroup_path = Path("/sys/fs/cgroup" + Path("/proc/self/cgroup").read_text().strip().split("::")[1])
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "method": "Alternating fresh processes, fresh seed copies, warm OS caches, persistent HTTP connection after first health response.",
        "limits": {name: (cgroup_path / name).read_text().strip()
                   for name in ("cpu.max", "memory.max", "memory.swap.max")},
        "variants": {label: {"directory": str(exe.parent),
                             "application_sha256": hashlib.sha256((exe.parent / "FeedApi.dll").read_bytes()).hexdigest(),
                             "publish_bytes": sum(path.stat().st_size for path in exe.parent.rglob("*") if path.is_file())}
                     for label, exe in variants.items()},
        "samples": [], "summary": {}
    }

    def request(connection, path, body=None, bearer=None):
        headers = {"Authorization": "Bearer " + bearer} if bearer else {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        start = time.perf_counter()
        connection.request("GET" if body is None else "POST", path, body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read()
        elapsed = (time.perf_counter() - start) * 1000
        assert response.status in (200, 201), (response.status, raw)
        return json.loads(raw), elapsed

    labels = list(variants)
    for iteration in range(args.runs):
        # Rotate order to spread background/thermal effects across variants.
        for label in labels[iteration % len(labels):] + labels[:iteration % len(labels)]:
            executable = variants[label]
            with tempfile.TemporaryDirectory(prefix="feed-startup-") as scratch:
                database = Path(scratch) / "feed.db"
                shutil.copyfile(args.seed, database)
                with socket.socket() as sock:
                    sock.bind(("127.0.0.1", 0))
                    port = sock.getsockname()[1]
                environment = dict(os.environ, SQLITE_PATH=str(database), JWT_SECRET="twelve-dollar-challenge",
                                   HOST="127.0.0.1", PORT=str(port))
                connection = None
                with (logs / f"{label}-{iteration + 1}.log").open("w") as log:
                    start = time.perf_counter()
                    process = subprocess.Popen([str(executable)], cwd="/", env=environment,
                                               stdout=log, stderr=subprocess.STDOUT)
                    try:
                        while True:
                            if process.poll() is not None:
                                raise RuntimeError(f"{label} exited at startup; see {log.name}")
                            if time.perf_counter() - start > 15:
                                raise TimeoutError(f"{label} did not become healthy")
                            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                            try:
                                request(connection, "/health")
                                break
                            except (ConnectionRefusedError, ConnectionResetError):
                                connection.close()
                                time.sleep(0.005)
                        ready = (time.perf_counter() - start) * 1000
                        feed, first_feed = request(connection, "/feed")
                        assert len(feed["posts"]) == 20
                        _, first_post = request(connection, f"/posts/{feed['posts'][0]['id']}")
                        bearer = token("twelve-dollar-challenge")
                        created, first_create = request(connection, "/posts", '{"body":"startup benchmark"}', bearer)
                        _, first_like = request(connection, f"/posts/{created['post']['id']}/like", "{}", bearer)
                        reads = [request(connection, "/feed")[1] for _ in range(20)]
                        status = Path(f"/proc/{process.pid}/status").read_text().splitlines()
                        rss_peak_kib = int(next(line.split()[1] for line in status if line.startswith("VmHWM:")))
                        sample = {"variant": label, "sample": iteration + 1, "ready_ms": ready,
                                  "first_feed_ms": first_feed, "first_post_ms": first_post,
                                  "first_create_ms": first_create, "first_like_ms": first_like,
                                  "warm_feed_median_ms": statistics.median(reads), "rss_peak_mib": rss_peak_kib / 1024}
                        report["samples"].append(sample)
                        print(json.dumps(sample), flush=True)
                    finally:
                        if connection:
                            connection.close()
                        if process.poll() is None:
                            process.terminate()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
    for label in labels:
        samples = [sample for sample in report["samples"] if sample["variant"] == label]
        report["summary"][label] = {key: {"median": statistics.median(sample[key] for sample in samples),
                                               "min": min(sample[key] for sample in samples),
                                               "max": max(sample[key] for sample in samples)}
                                     for key in samples[0] if key not in ("variant", "sample")}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("SUMMARY " + json.dumps(report["summary"]), flush=True)


if __name__ == "__main__":
    main()
