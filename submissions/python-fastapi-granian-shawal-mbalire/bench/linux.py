#!/usr/bin/env python3
"""Benchmarks the submission in Docker with the droplet's limits, like the challenge's scoring run.

Server container: Ubuntu 24.04 built with our own install.sh and build.sh, one dedicated CPU
(--cpuset-cpus 0), 1792 MiB RAM and no swap (2 GB minus what an idle droplet's OS uses), and
nofile 65535. The load generator (bench/loadgen.py, a load.js clone) runs in its own container
on the other CPUs, standing in for the separate k6 machine.

    just vm start                            # once: the Docker VM (Colima profile `bench`)
    just bench linux 20000                   # one run: 60 s ramp + 300 s hold
    just bench linux 20000 --duration 60     # shorter hold while exploring
    just bench ladder                        # the scoring ladder: 2,500 doubling, then narrow to 250

Stdlib only and Python 3.9 compatible, so the system python3 can run it.
"""

import argparse
import json
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parent.parent
WORK = ROOT / "build" / "linuxbench"
SERVER_IMAGE, GEN_IMAGE = "twelve-server", "twelve-loadgen"
SERVER, GEN = "twelve-server", "twelve-loadgen"
SKIP = shutil.ignore_patterns(".venv", ".devtools", "build", "__pycache__", "bench", ".git")

SERVER_DOCKERFILE = """\
FROM ubuntu:24.04
COPY app /app
RUN bash /app/install.sh && useradd --create-home runner && chown -R runner /app
COPY --chown=runner feed.db /home/runner/feed.db
USER runner
RUN bash /app/build.sh
ENV SQLITE_PATH=/home/runner/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000
CMD ["bash", "/app/start.sh"]
"""
GEN_DOCKERFILE = """\
FROM python:3.12-slim
RUN pip install --no-cache-dir uvloop==0.23.0
COPY loadgen.py tokens.json /gen/
WORKDIR /gen
"""


def sh(*cmd, check=True, capture=False):
    return subprocess.run(cmd, check=check, text=True, capture_output=capture)


def build_images() -> None:
    server_ctx, gen_ctx = WORK / "server", WORK / "loadgen"
    shutil.rmtree(server_ctx, ignore_errors=True)
    shutil.copytree(ROOT, server_ctx / "app", ignore=SKIP)
    shutil.copy(REPO / "seed" / "feed.db", server_ctx / "feed.db")
    (server_ctx / "Dockerfile").write_text(SERVER_DOCKERFILE)
    gen_ctx.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / "loadgen.py", gen_ctx / "loadgen.py")
    shutil.copy(REPO / "seed" / "tokens.json", gen_ctx / "tokens.json")
    (gen_ctx / "Dockerfile").write_text(GEN_DOCKERFILE)
    print("== building images", flush=True)
    sh("docker", "build", "-q", "-t", SERVER_IMAGE, str(server_ctx))
    sh("docker", "build", "-q", "-t", GEN_IMAGE, str(gen_ctx))


def start_server() -> None:
    sh("docker", "rm", "-f", SERVER, check=False, capture=True)
    sh("docker", "run", "-d", "--name", SERVER, "--network", "host",
       "--cpuset-cpus", "0", "--memory", "1792m", "--memory-swap", "1792m",
       "--ulimit", "nofile=65535:65535", SERVER_IMAGE, capture=True)
    probe = "import urllib.request; urllib.request.urlopen('http://127.0.0.1:3000/health', timeout=1)"
    for _ in range(60):
        if sh("docker", "run", "--rm", "--network", "host", "--cpuset-cpus", "1", GEN_IMAGE,
              "python", "-c", probe, check=False, capture=True).returncode == 0:
            return
        time.sleep(1)
    print(sh("docker", "logs", "--tail", "30", SERVER, check=False, capture=True).stderr)
    raise SystemExit("server not healthy within 60 s")


def cgroup(path: str) -> str:
    # Read from inside the container's cgroup: one `cat`, after the load has finished.
    return sh("docker", "exec", SERVER, "cat", "/sys/fs/cgroup/" + path, check=False, capture=True).stdout


def watch_memory(samples: list, stop: threading.Event) -> None:
    while not stop.wait(5):
        out = sh("docker", "stats", "--no-stream", "--format", "{{.MemUsage}}|{{.CPUPerc}}", SERVER,
                 check=False, capture=True).stdout.strip()
        if out:
            samples.append(out)


def run(vus: int, ramp: int, duration: int) -> dict:
    start_server()
    samples, stop = [], threading.Event()
    watcher = threading.Thread(target=watch_memory, args=(samples, stop), daemon=True)
    watcher.start()
    cpu_before = cgroup("cpu.stat")
    print(f"== {vus} users: {ramp}s ramp + {duration}s hold", flush=True)
    gen = subprocess.run(
        ["docker", "run", "--rm", "--name", GEN, "--network", "host", "--cpuset-cpus", "1-7",
         "--ulimit", "nofile=1048576:1048576", GEN_IMAGE, "python", "loadgen.py",
         "--vus", str(vus), "--ramp", str(ramp), "--duration", str(duration), "--procs", "7"],
        stdout=subprocess.PIPE, text=True)
    stop.set()
    lines = gen.stdout.strip().splitlines()
    if gen.returncode != 0 or not lines:
        raise SystemExit("load generator failed")
    result = json.loads(lines[-1])

    def usage_s(stat: str) -> float:
        for line in stat.splitlines():
            if line.startswith("usage_usec"):
                return int(line.split()[1]) / 1e6
        return 0.0

    result["server_cpu_s"] = round(usage_s(cgroup("cpu.stat")) - usage_s(cpu_before), 1)
    result["server_cpu_util"] = round(result["server_cpu_s"] / (ramp + duration), 3)
    peak = cgroup("memory.peak").strip()
    result["server_mem_peak_mib"] = round(int(peak) / 2**20, 1) if peak.isdigit() else None
    result["server_oom_killed"] = sh("docker", "inspect", "-f", "{{.State.OOMKilled}}", SERVER,
                                     capture=True).stdout.strip() == "true"
    result["server_samples"] = samples[-3:]
    if result["server_oom_killed"] or not result["pass"]:
        result["pass"] = False
        print(sh("docker", "logs", "--tail", "15", SERVER, check=False, capture=True).stderr, file=sys.stderr)
    sh("docker", "rm", "-f", SERVER, capture=True)
    WORK.mkdir(parents=True, exist_ok=True)
    with open(WORK / "results.jsonl", "a") as f:
        f.write(json.dumps(result) + "\n")
    print(json.dumps(result), flush=True)
    return result


def ladder(ramp: int, duration: int) -> None:
    """2,500 users, doubling until a run fails, then bisect to within 250, then confirm."""
    good, bad, vus = 0, None, 2500
    while bad is None:
        if run(vus, ramp, duration)["pass"]:
            good, vus = vus, vus * 2
        else:
            bad = vus
    while bad - good > 250:
        mid = (good + bad) // 2 // 250 * 250
        if run(mid, ramp, duration)["pass"]:
            good = mid
        else:
            bad = mid
    print(f"== limit: {good} users pass, {bad} fail", flush=True)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["run", "ladder"])
    p.add_argument("vus", type=int, nargs="?", default=2500)
    p.add_argument("--ramp", type=int, default=60)
    p.add_argument("--duration", type=int, default=300)
    p.add_argument("--no-build", action="store_true")
    args = p.parse_args()
    if sh("docker", "info", check=False, capture=True).returncode != 0:
        raise SystemExit("docker is not reachable: run `just vm start` (or start your own Docker) first")
    if not args.no_build:
        build_images()
    if args.mode == "run":
        run(args.vus, args.ramp, args.duration)
    else:
        ladder(args.ramp, args.duration)


if __name__ == "__main__":
    main()
