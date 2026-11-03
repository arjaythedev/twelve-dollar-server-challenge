#!/usr/bin/env python3
"""Rotating four-stack Linux comparison with fresh databases and raw evidence.

Run --help for the host-side command. The serve subcommand runs inside the server container.
Both containers mount this repository at /work and share a network namespace.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CONTAINER_DIR = "/work/submissions/" + HERE.name


def metrics(pid):
    status = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmHWM:", "Threads:")):
            key, value = line.split(":", 1)
            status[key] = int(value.split()[0])
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    status["cpu_seconds"] = (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK")
    return status


def serve(args):
    with tempfile.TemporaryDirectory(prefix="twelve-v2-bench-") as directory:
        directory = Path(directory)
        shutil.copyfile(ROOT / "seed/feed.db", directory / "feed.db")
        env = dict(os.environ, SQLITE_PATH=str(directory / "feed.db"), JWT_SECRET="twelve-dollar-challenge",
                   HOST="127.0.0.1", PORT=str(args.port))
        with (directory / "server.log").open("w+") as log:
            process = subprocess.Popen([args.binary], env=env, stdout=log, stderr=log)
            try:
                for _ in range(200):
                    if process.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    try:
                        urllib.request.urlopen(f"http://127.0.0.1:{args.port}/health", timeout=1).read()
                        break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("server startup timeout")
                print(json.dumps({"ready": True, "binary_sha256": hashlib.sha256(Path(args.binary).read_bytes()).hexdigest()}), flush=True)
                for command in sys.stdin:
                    print(json.dumps(metrics(process.pid)), flush=True)
                    if command.strip() == "stop":
                        break
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def run(args):
    records = []
    binaries = dict(item.split("=", 1) for item in args.binaries.split(","))
    workloads = {"feed": "/feed", "post": "/posts/500000", "mixed": "/feed"}
    result = {"configuration": vars(args), "runs": records}
    if Path(args.output).exists():
        raise RuntimeError("output already exists; choose a new filename to retain previous evidence")
    for workload in args.workloads.split(","):
        for trial in range(args.trials):
            names = list(binaries)
            offset = trial % len(names)
            order = names[offset:] + names[:offset]
            if trial % 2: order.reverse()
            for name in order:
                port = args.port_base + len(records)
                server = subprocess.Popen([
                    "docker", "exec", "-i", args.server_container, "python3", CONTAINER_DIR + "/compare.py",
                    "serve", "--binary", binaries[name], "--port", str(port)
                ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    ready = server.stdout.readline()
                    if not ready or not json.loads(ready).get("ready"):
                        raise RuntimeError(ready + server.stderr.read())
                    ready = json.loads(ready)
                    wrk = ["docker", "exec", "-w", "/work", args.load_container, "wrk", "-t2", "-c64", "--latency"]
                    if workload == "mixed":
                        wrk += ["-s", CONTAINER_DIR + "/bench.lua"]
                    url = f"http://127.0.0.1:{port}" + workloads[workload]
                    subprocess.run(wrk + ["-d2s", url], check=True, stdout=subprocess.DEVNULL)
                    server.stdin.write("measure\n")
                    server.stdin.flush()
                    before = json.loads(server.stdout.readline())
                    started = time.monotonic()
                    output = subprocess.check_output(wrk + [f"-d{args.seconds}s", url], text=True, timeout=args.seconds + 20)
                    elapsed = time.monotonic() - started
                    server.stdin.write("stop\n")
                    server.stdin.flush()
                    after = json.loads(server.stdout.readline())
                    _, error = server.communicate(timeout=15)
                    if server.returncode:
                        raise RuntimeError(error)
                    duration = float(re.search(r" requests in ([0-9.]+)s,", output)[1])
                    record = dict(name=name, workload=workload, trial=trial + 1, before=before, metrics=after,
                                  binary_sha256=ready["binary_sha256"], host_elapsed=elapsed, wrk_duration=duration,
                                  requests_per_second=float(re.search(r"Requests/sec:\s+([0-9.]+)", output)[1]), output=output)
                    records.append(record)
                    Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
                    print(workload, trial + 1, name, record["requests_per_second"], "req/s", after["VmRSS"], "KiB RSS", flush=True)
                    if "Non-2xx" in output or "Socket errors" in output:
                        raise RuntimeError("benchmark errors; failed run retained in output\n" + output)
                    if not args.seconds - 0.5 <= duration <= args.seconds + 2 or elapsed > args.seconds + 4:
                        raise RuntimeError("unexpected measurement duration; run retained in output")
                finally:
                    if server.poll() is None:
                        try:
                            server.communicate(input="stop\n", timeout=15)
                        except subprocess.TimeoutExpired:
                            server.kill()
                            server.wait()


parser = argparse.ArgumentParser(description=__doc__)
commands = parser.add_subparsers(dest="command", required=True)
server_parser = commands.add_parser("serve")
server_parser.add_argument("--binary", required=True)
server_parser.add_argument("--port", type=int, required=True)
run_parser = commands.add_parser("run")
run_parser.add_argument("--binaries", default=",".join(name + "=/work/submissions/" + folder + "/bin/server" for name, folder in [
    ("cpp", "cpp-uwebsockets-v2-tbsvttr"), ("fortran", "fortran-microhttpd-tbsvttr"),
    ("rust", "rust-axum-tbsvttr"), ("go", "go-nethttp-tbsvttr")]), help="comma-separated name=/absolute/binary pairs inside server container")
run_parser.add_argument("--server-container", default="twelve-language-server")
run_parser.add_argument("--load-container", default="twelve-language-loadgen")
run_parser.add_argument("--workloads", default="mixed")
run_parser.add_argument("--trials", type=int, default=5)
run_parser.add_argument("--seconds", type=int, default=15)
run_parser.add_argument("--port-base", type=int, default=20000)
run_parser.add_argument("--output", required=True)
args = parser.parse_args()
serve(args) if args.command == "serve" else run(args)
