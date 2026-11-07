#!/usr/bin/env python3
"""Focused, alternating local A/B for the SQLite feed B-tree path."""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def client_main(argv: list[str]) -> int:
    port, duration, workers, cpu, path = int(argv[0]), float(argv[1]), int(argv[2]), int(argv[3]), argv[4]
    request = f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n".encode()

    async def one(deadline: float) -> tuple[int, int, float]:
        count = errors = 0
        start = time.monotonic()
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            while time.monotonic() < deadline:
                writer.write(request)
                await writer.drain()
                headers = await reader.readuntil(b"\r\n\r\n")
                if b" 200 " not in headers.split(b"\r\n", 1)[0]:
                    errors += 1
                length = next(
                    (int(line.split(b":", 1)[1]) for line in headers.split(b"\r\n")
                     if line.lower().startswith(b"content-length:")),
                    None,
                )
                if length is None:
                    errors += 1
                    break
                body = await reader.readexactly(length)
                shape = body.startswith(b'{"posts":[') if path == "/feed" else body.startswith(b'{"post":{')
                if not shape:
                    errors += 1
                count += 1
        except Exception:
            errors += 1
        finally:
            if "writer" in locals():
                writer.close()
                try:
                    await writer.wait_closed()
                except Exception:
                    pass
        return count, errors, time.monotonic() - start

    async def run() -> None:
        deadline = time.monotonic() + duration
        results = await asyncio.gather(*(one(deadline) for _ in range(workers)))
        total = sum(result[0] for result in results)
        elapsed = max(result[2] for result in results)
        print(json.dumps({
            "requests": total,
            "errors": sum(result[1] for result in results),
            "elapsed_s": elapsed,
            "requests_per_s": total / elapsed if elapsed else 0,
        }))

    asyncio.run(run())
    return 0


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    parser.add_argument("--seed", type=pathlib.Path, required=True)
    parser.add_argument("--server-cpu", type=int, default=2)
    parser.add_argument("--client-cpus", default="3,4,5,6")
    parser.add_argument("--workers-per-client", type=int, default=16)
    parser.add_argument("--warmup", type=float, default=3)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--workloads", default="feed,post")
    parser.add_argument("--port", type=int, default=3321)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("/tmp/btree-ab.json"))
    args = parser.parse_args()
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    seed = args.seed.resolve()
    if not seed.is_file() or any(not binary.is_file() for binary in binaries.values()):
        parser.error("seed and both executable binaries must exist")
    client_cpus = [int(cpu) for cpu in args.client_cpus.split(",")]
    workloads = ["/feed" if w == "feed" else "/posts/500000" if w == "post" else "" for w in args.workloads.split(",")]
    if not workloads or any(not path for path in workloads) or not client_cpus:
        parser.error("workloads must be feed and/or post; client-cpus cannot be empty")

    metadata = {
        "seed_sha256": sha256(seed),
        "binary_sha256": {name: sha256(binary) for name, binary in binaries.items()},
        "server_cpu": args.server_cpu,
        "client_cpus": client_cpus,
        "workers_per_client": args.workers_per_client,
        "warmup_s": args.warmup,
        "duration_s": args.duration,
        "trials": args.trials,
        "workloads": workloads,
        "server_mode": "epoll",
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    results: list[dict[str, object]] = []
    here = pathlib.Path(__file__).resolve()

    def load(seconds: float, path: str) -> dict[str, object]:
        commands = [
            ["taskset", "-c", str(cpu), sys.executable, str(here), "--client",
             str(args.port), str(seconds), str(args.workers_per_client), str(cpu), path]
            for cpu in client_cpus
        ]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                     for command in commands]
        parsed = []
        for process in processes:
            output, error = process.communicate(timeout=seconds + 20)
            if process.returncode:
                raise RuntimeError(f"load client failed: {error or output}")
            parsed.append(json.loads(output))
        elapsed = max(float(row["elapsed_s"]) for row in parsed)
        requests = sum(int(row["requests"]) for row in parsed)
        errors = sum(int(row["errors"]) for row in parsed)
        return {"requests": requests, "errors": errors, "elapsed_s": elapsed,
                "requests_per_s": requests / elapsed if elapsed else 0}

    for trial in range(args.trials):
        ordered_workloads = workloads[trial % len(workloads):] + workloads[:trial % len(workloads)]
        for workload_index, path in enumerate(ordered_workloads):
            order = ["baseline", "candidate"] if (trial + workload_index) % 2 == 0 else ["candidate", "baseline"]
            for name in order:
                with tempfile.TemporaryDirectory(prefix="twelve-btree-ab-") as directory:
                    db = pathlib.Path(directory) / "feed.db"
                    shutil.copyfile(seed, db)
                    log_path = pathlib.Path(directory) / "server.log"
                    env = os.environ | {
                        "SQLITE_PATH": str(db), "JWT_SECRET": "twelve-dollar-challenge",
                        "HOST": "127.0.0.1", "PORT": str(args.port),
                    }
                    with log_path.open("w") as log:
                        server = subprocess.Popen(["taskset", "-c", str(args.server_cpu),
                                                   str(binaries[name]), "--epoll"],
                                                  env=env, stdout=log, stderr=subprocess.STDOUT)
                        try:
                            for _ in range(1200):
                                try:
                                    urllib.request.urlopen(f"http://127.0.0.1:{args.port}/health", timeout=0.2).read()
                                    break
                                except Exception:
                                    if server.poll() is not None:
                                        raise RuntimeError(f"server exited: {log_path.read_text()}")
                                    time.sleep(0.1)
                            else:
                                raise RuntimeError(f"server did not become healthy: {log_path.read_text()}")
                            warmup = load(args.warmup, path)
                            measured = load(args.duration, path)
                            row = {"trial": trial + 1, "variant": name, "path": path,
                                   "warmup": warmup, "measured": measured,
                                   "server_log": log_path.read_text()}
                            results.append(row)
                            print(json.dumps(row), flush=True)
                            if warmup["errors"] or measured["errors"]:
                                raise RuntimeError(f"request/status/body errors: {row}")
                        finally:
                            server.terminate()
                            try:
                                server.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                server.kill()
                                server.wait()
    output = {"metadata": metadata, "results": results}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2) + "\n")
    for path in workloads:
        medians = {}
        for name in binaries:
            rates = [float(row["measured"]["requests_per_s"]) for row in results
                     if row["path"] == path and row["variant"] == name]
            rates.sort()
            median = rates[len(rates) // 2] if len(rates) % 2 else sum(rates[len(rates)//2-1:len(rates)//2+1]) / 2
            medians[name] = median
            print(f"{path} {name}: median {median:.1f} req/s")
        paired = []
        for trial in range(1, args.trials + 1):
            trial_rows = {str(row["variant"]): float(row["measured"]["requests_per_s"])
                          for row in results if row["path"] == path and row["trial"] == trial}
            if "baseline" in trial_rows and "candidate" in trial_rows:
                paired.append(trial_rows["candidate"] / trial_rows["baseline"] - 1)
        paired.sort()
        paired_median = paired[len(paired) // 2] if len(paired) % 2 else sum(paired[len(paired)//2-1:len(paired)//2+1]) / 2
        print(f"{path} paired median change: {paired_median*100:+.2f}%")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--client":
        raise SystemExit(client_main(sys.argv[2:]))
    raise SystemExit(main())
