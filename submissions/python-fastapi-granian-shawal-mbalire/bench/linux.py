#!/usr/bin/env python3
"""Benchmarks a submission in Docker with the droplet's limits, like the challenge's scoring run.

One Linux VM (Colima profile `bench`, 8 CPUs), with the containers declared in Terraform
(infra/iac/bench). The server container gets CPU 0, 1792 MiB of RAM and no swap (2 GB minus what an
idle droplet's OS uses), and nofile 65535. Four load generator containers split CPUs 1-7, standing in
for the separate k6 machine; each has its own address, so its own ~64k ports to the server.

The two talk over a Docker bridge network, not loopback, and the kernel's network work is steered
the way two machines would split it (see steer_network): packets the server receives are processed
on CPU 0, packets a generator receives on that generator's CPUs. Over loopback most of the server's TCP cost
landed on the generator's CPUs, and the server looked about 3x cheaper than PR #71 measured it on
two real VMs. `core0_util` (all of CPU 0's busy time, network included) is the number to watch.

An M-series core is faster than a droplet vCPU, and a CPU cap doesn't model that: at --cpus 0.13 the
REFS entries reached 1k, 3k and 11-16k users against droplet scores of 11k, 22k and 22k (throttling
hurts Python far more than Go). Compare entries and changes by core0_util per user, uncapped; at
their droplet scores the REFS entries used 19-26% of CPU 0 (AGENTS.md has the numbers).

    just vm start                              # once: the Docker VM
    just bench linux 20000                     # one run: 60 s ramp + 300 s hold
    just bench linux 20000 --duration 60       # shorter hold while exploring
    just bench linux 20000 --cpus 0.5          # with the server's CPU capped
    just bench linux 11000 --submission cknutson12   # another entry (see REFS)
    just bench ladder --cpus 0.5               # the scoring ladder: 2,500 doubling, then narrow to 250
    just bench calibrate --cpus 0.5            # every REFS entry's limit next to its droplet score (see above)
    just bench memory                          # server memory per idle keep-alive connection
    just bench linux 20000 --gen python        # bench/loadgen.py instead of PR #14's C generator
    just bench memory --granian=--http1-buffer-size=8192   # extra Granian flags (this entry only)

Four generator addresses give about 256k connections, so runs can pass 64k users; the server still
has the droplet's 65,535-descriptor limit and retires connections near it.
Stdlib only and Python 3.9 compatible, so the system python3 can run it.
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parent.parent
WORK = ROOT / "build" / "linuxbench"
UPSTREAM = "https://github.com/arjaythedev/twelve-dollar-server-challenge.git"
IAC = ROOT / "infra" / "iac" / "bench"  # the Terraform for the containers and the network
TERRAFORM_STATE = WORK / "terraform"  # .terraform and the state file stay out of the source tree
SERVER, SERVER_IP, PORT, GEN_IMAGE = "twelve-server", "172.30.0.10", 3000, "twelve-loadgen"
# One generator container per CPU set; names match infra/iac/bench/main.tf.
GENERATOR_CPU_SETS = ["1-2", "3-4", "5-6", "7"]
GENERATORS = [f"twelve-loadgen-{index}" for index in range(len(GENERATOR_CPU_SETS))]
SERVER_CPUMASK = "1"  # CPU 0


def cpus_in(cpu_set: str) -> list:
    first, _, last = cpu_set.partition("-")
    return list(range(int(first), int(last or first) + 1))


def cpumask(cpu_set: str) -> str:
    return format(sum(1 << cpu for cpu in cpus_in(cpu_set)), "x")


# where the server's sent packets ("the wire") are processed: every generator CPU
GENERATORS_CPUMASK = format(sum(int(cpumask(cpu_set), 16) for cpu_set in GENERATOR_CPU_SETS), "x")
# One generator process per generator CPU: (container, CPU set) for each.
GENERATOR_SLOTS = [(name, cpu_set) for name, cpu_set in zip(GENERATORS, GENERATOR_CPU_SETS)
                   for _ in cpus_in(cpu_set)]
SKIP = shutil.ignore_patterns(".venv", ".devtools", "build", "__pycache__", "bench", ".git", "node_modules")

# Entries with a known droplet score (from the challenge video, as listed in PR #71's README). `sha`
# pins an unmerged PR's revision, fetched from upstream; None means the folder in this checkout.
# The video may have scored an earlier revision than the one pinned here.
REFS = {
    "cknutson12": {"folder": "python-fastapi-cknutson12", "sha": None, "droplet": 11000, "pr": 1},
    "tejasnafde": {"folder": "python-fastapi-tejasnafde", "pr": 10, "droplet": 22000,
                   "sha": "606970b8c9c85bcebf299f7208a99525d4d6aa23"},
    "go-tbsvttr": {"folder": "go-nethttp-tbsvttr", "pr": 18, "droplet": 22000,
                   "sha": "a3845fc52677b55394d5ff2045f5945d2c3ba273"},
    # Not scored on a droplet (droplet None): compared by core0_util only, skipped by calibrate.
    "granian-tbsvttr": {"folder": "python-granian-tbsvttr", "pr": 13, "droplet": None,
                        "sha": "825048080634a9b43d329b6597623ffe34d3c240"},
    "pypy-pranavds": {"folder": "python-pypy-epoll-pranavds-codes", "pr": 84, "droplet": None,
                      "sha": "87b92833c14d2a993b65a38c429512200dbb4fc5"},
    "ruby-cicloid": {"folder": "ruby-async-cicloid", "pr": 71, "droplet": None,
                     "sha": "469957f2bbdd6e7483d567c860378031154e6ac9"},
}

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

# PR #14's C load generator (load.js mix and think times, epoll, one process per core). PR #71 used it
# to compare entries. Fetched at a pinned commit rather than copied into this folder.
CGEN_URL = ("https://raw.githubusercontent.com/tbsvttr/twelve-dollar-server-challenge/"
            "4eaabd8ff706997bd5e9e211ab85c78d1dcfad5e/submissions/ruby-iodine-tbsvttr/tests/connection_load.c")
CGEN_SHA256 = "311676da3a505216316092673a156291522ebff318ac6b5a263f76f51bf40056"
# It only targets 127.0.0.1 (from 127.1.x.y source addresses); these edits add a TARGET_HOST variable.
CGEN_PATCH = [
    ("static double loop_delay;", "static double loop_delay;\nstatic in_addr_t target_address;"),
    ("port = atoi(argv[1]);", "port = atoi(argv[1]);\n    target_address = getenv(\"TARGET_HOST\") ? "
     "inet_addr(getenv(\"TARGET_HOST\")) : htonl(INADDR_LOOPBACK);"),
    (".sin_addr.s_addr = htonl(INADDR_LOOPBACK)};", ".sin_addr.s_addr = target_address};"),
    ("source.sin_addr.s_addr = htonl(", "source.sin_addr.s_addr = target_address != htonl(INADDR_LOOPBACK) ? "
     "htonl(INADDR_ANY) : htonl("),
]
# One image for every generator: bench/loadgen.py, bench/idleconns.py and the C generator.
GEN_DOCKERFILE = """\
FROM python:3.12-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev >/dev/null \\
    && pip install --no-cache-dir uvloop==0.23.0
COPY connection_load.c loadgen.py idleconns.py tokens.json /gen/
RUN cc -O3 -Wall -Wextra /gen/connection_load.c -lm -o /usr/local/bin/connection-load
WORKDIR /gen
"""


# ~/.docker/config.json may name Docker Desktop's credential helper (credsStore: desktop), which pulls
# need even for public images; it lives in the app bundle, not always on PATH.
DESKTOP_BIN = "/Applications/Docker.app/Contents/Resources/bin"
if Path(DESKTOP_BIN).is_dir() and DESKTOP_BIN not in os.environ.get("PATH", ""):
    os.environ["PATH"] = os.environ.get("PATH", "") + os.pathsep + DESKTOP_BIN


def sh(*cmd, check=True, capture=False):
    return subprocess.run(cmd, check=check, text=True, capture_output=capture)


# --- images -----------------------------------------------------------------------------------------

def submission_dir(name: str) -> Path:
    if name == "ours":
        return ROOT
    ref = REFS[name]
    if ref["sha"] is None:
        return REPO / "submissions" / ref["folder"]
    dest = WORK / "refs" / ref["sha"]
    if not (dest / "submissions" / ref["folder"]).is_dir():
        sh("git", "-C", str(REPO), "fetch", "-q", "--depth", "1", UPSTREAM, ref["sha"])
        archive = subprocess.run(["git", "-C", str(REPO), "archive", ref["sha"], "submissions/" + ref["folder"]],
                                 check=True, capture_output=True).stdout
        dest.mkdir(parents=True, exist_ok=True)
        tarfile.open(fileobj=io.BytesIO(archive)).extractall(dest)
    return dest / "submissions" / ref["folder"]


def server_image(name: str) -> str:
    return "twelve-server-" + name


def build_server_image(name: str) -> None:
    ctx = WORK / "server" / name
    shutil.rmtree(ctx, ignore_errors=True)
    shutil.copytree(submission_dir(name), ctx / "app", ignore=SKIP)
    shutil.copy(REPO / "seed" / "feed.db", ctx / "feed.db")
    (ctx / "Dockerfile").write_text(SERVER_DOCKERFILE)
    print(f"== building {name}", flush=True)
    sh("docker", "build", "-q", "-t", server_image(name), str(ctx), capture=True)


def build_gen_image() -> None:
    ctx = WORK / "loadgen"
    ctx.mkdir(parents=True, exist_ok=True)
    source = urllib.request.urlopen(CGEN_URL, timeout=30).read()
    if hashlib.sha256(source).hexdigest() != CGEN_SHA256:
        raise SystemExit("connection_load.c does not match its pinned SHA-256")
    text = source.decode()
    for old, new in CGEN_PATCH:
        if text.count(old) != 1:
            raise SystemExit(f"connection_load.c patch does not apply: {old!r}")
        text = text.replace(old, new)
    (ctx / "connection_load.c").write_text(text)
    for name in ("loadgen.py", "idleconns.py"):
        shutil.copy(HERE / name, ctx / name)
    shutil.copy(REPO / "seed" / "tokens.json", ctx / "tokens.json")
    (ctx / "Dockerfile").write_text(GEN_DOCKERFILE)
    sh("docker", "build", "-q", "-t", GEN_IMAGE, str(ctx), capture=True)


# --- containers and the network ---------------------------------------------------------------------

def terraform(*args: str) -> None:
    env = {**os.environ, "TF_DATA_DIR": str(TERRAFORM_STATE / ".terraform"), "TF_IN_AUTOMATION": "1"}
    subprocess.run(["terraform", f"-chdir={IAC}", *args], check=True, env=env, stdout=subprocess.DEVNULL)


def terraform_init() -> None:
    if not (TERRAFORM_STATE / ".terraform").is_dir():
        TERRAFORM_STATE.mkdir(parents=True, exist_ok=True)
        terraform("init", "-input=false", f"-backend-config=path={TERRAFORM_STATE / 'bench.tfstate'}")


def docker_host() -> str:
    return sh("docker", "context", "inspect", "-f", "{{.Endpoints.docker.Host}}", capture=True).stdout.strip()


def terraform_variables(name: str, cpus, granian_flags) -> list:
    """infra/iac/bench's variables for one run, as -var flags."""
    variables = {
        "docker_host": docker_host(), "server_image": server_image(name), "generator_image": GEN_IMAGE,
        "server_command": json.dumps(server_command(name, granian_flags)),
        "server_cpu_quota": int(cpus * 10000) if cpus else 0,  # per 10 ms period
        "generator_cpu_sets": json.dumps(GENERATOR_CPU_SETS), "server_ip": SERVER_IP,
    }
    return [f"-var={key}={value}" for key, value in variables.items()]


def server_command(name: str, granian_flags) -> list:
    """Extra Granian flags are appended to infra/ops.py's GRANIAN_FLAGS; for repeated flags the last one wins."""
    if not granian_flags:
        return []
    if name != "ours":
        raise SystemExit("--granian applies to this entry only")
    return ["python3", "-c", "import sys; sys.path.insert(0, 'infra'); import ops; "
            "ops.GRANIAN_FLAGS += sys.argv[1:]; ops.start()", *granian_flags]


def set_rps(container: str, receive_cpumask: str, wire_cpumask: str) -> None:
    """Steers where the kernel processes the packets `container` receives and sends.

    veth hands each packet to the receiver on the CPU that sent it, so by default the server's CPU did
    the generator's receive work and the other way round. With RPS, what a container receives (its
    eth0) is processed on `receive_cpumask`, and what it sends (its veth peer on the bridge, i.e. the
    "wire" and the bridge) on `wire_cpumask`, a generator's CPUs, so CPU 0 does only what a droplet's
    vCPU would.
    """
    peer = sh("docker", "run", "--rm", "--privileged", "--network", "container:" + container, GEN_IMAGE, "sh", "-c",
              # joining another container's network namespace mounts /sys read-only, even privileged
              f"set -e; mount -o remount,rw /sys; for q in /sys/class/net/eth0/queues/rx-*; do echo {receive_cpumask} > $q/rps_cpus; done; "
              "cat /sys/class/net/eth0/iflink", capture=True).stdout.strip()
    sh("docker", "run", "--rm", "--privileged", "--network", "host", GEN_IMAGE, "sh", "-c",
       f"for d in /sys/class/net/*; do [ \"$(cat $d/ifindex)\" = {peer} ] && "
       f"for q in $d/queues/rx-*; do echo {wire_cpumask} > $q/rps_cpus; done; done; true", capture=True)


def start_all(name: str, cpus, granian_flags=()) -> None:
    """A fresh server (and database) and fresh generators for every run."""
    stop_all()
    terraform("apply", "-input=false", "-auto-approve", *terraform_variables(name, cpus, granian_flags))
    set_rps(SERVER, SERVER_CPUMASK, GENERATORS_CPUMASK)
    for generator, cpu_set in zip(GENERATORS, GENERATOR_CPU_SETS):
        set_rps(generator, cpumask(cpu_set), cpumask(cpu_set))
    wait_healthy()


def wait_healthy() -> None:
    probe = f"import urllib.request; urllib.request.urlopen('http://{SERVER_IP}:{PORT}/health', timeout=1)"
    for _ in range(60):
        if sh("docker", "exec", GENERATORS[0], "python", "-c", probe, check=False, capture=True).returncode == 0:
            return
        time.sleep(1)
    print(sh("docker", "logs", "--tail", "30", SERVER, check=False, capture=True).stderr)
    raise SystemExit("server not healthy within 60 s")


def stop_all() -> None:
    terraform_init()
    # containers from before the bench used Terraform
    sh("docker", "rm", "-f", "twelve-loadgen", check=False, capture=True)
    terraform("destroy", "-input=false", "-auto-approve", *terraform_variables("ours", None, ()))
    sh("docker", "network", "rm", "twelve-net", check=False, capture=True)


def cat(*paths: str) -> str:
    """Files read inside the server container; /proc/stat and /proc/meminfo are the VM's."""
    return sh("docker", "exec", SERVER, "cat", *paths, check=False, capture=True).stdout


def core0() -> list:
    """CPU 0's jiffies: user nice system idle iowait irq softirq steal."""
    for line in cat("/proc/stat").splitlines():
        if line.startswith("cpu0 "):
            return [int(x) for x in line.split()[1:9]]
    return [0] * 8


# --- generators -------------------------------------------------------------------------------------

def python_gen(vus: int, ramp: int, duration: int) -> dict:
    """bench/loadgen.py on the first generator only (its 2 CPUs): one IP, so at most ~64k users."""
    gen = subprocess.run(["docker", "exec", GENERATORS[0], "python", "loadgen.py", "--host", SERVER_IP,
                          "--vus", str(vus), "--ramp", str(ramp), "--duration", str(duration),
                          "--procs", str(len(cpus_in(GENERATOR_CPU_SETS[0])))],
                         stdout=subprocess.PIPE, text=True)
    lines = gen.stdout.strip().splitlines()
    if gen.returncode != 0 or not lines:
        raise SystemExit("load generator failed")
    return json.loads(lines[-1])


def c_gen(vus: int, ramp: int, duration: int) -> dict:
    """One process per generator CPU, each with its share of the users; worst-process percentiles, like PR #71."""
    share = -(-vus // len(GENERATOR_SLOTS))
    procs = [subprocess.Popen(["docker", "exec", "-e", "TARGET_HOST=" + SERVER_IP, generator, "connection-load", str(PORT),
                               "--users", str(min(share, vus - i * share)), "--offset", str(i * share),
                               "--seconds", str(duration), "--ramp", str(ramp), "--tokens", "tokens.json"],
                              stdout=subprocess.PIPE, text=True)
             for i, (generator, _) in enumerate(GENERATOR_SLOTS) if i * share < vus]
    parts = []
    for proc in procs:
        lines = proc.communicate()[0].strip().splitlines()
        if not lines or "diagnostic" not in lines[-1]:
            raise SystemExit("C load generator failed")
        parts.append(json.loads(lines[-1]))
    requests = sum(p["requests"] for p in parts)
    failed = sum(p["errors"]["failed_requests"] for p in parts)
    return {
        "vus": vus, "requests": requests, "rps": round(sum(p["requests_per_second"] for p in parts), 1),
        "error_rate": round(failed / requests, 5) if requests else 1.0,
        "p95_ms": max(p["p95_ms"] for p in parts), "p99_ms": max(p["p99_ms"] for p in parts),
        "end_to_end_p99_ms": max(p["end_to_end_p99_ms"] for p in parts),
        "gen_loop_delay_ms": max(p["event_loop_delay_ms"] for p in parts),  # the generator running late
        "peak_connections": sum(p["peak_connections"] for p in parts),
        "connects": sum(p["connects"] for p in parts), "retired": sum(p["retired_connections"] for p in parts),
        "pass": all(p["pass"] for p in parts),
    }


# --- modes ------------------------------------------------------------------------------------------

def watch_memory(samples: list, stop: threading.Event) -> None:
    while not stop.wait(5):
        out = sh("docker", "stats", "--no-stream", "--format", "{{.MemUsage}}", SERVER,
                 check=False, capture=True).stdout.strip()
        if out:
            samples.append(out)


SERVER_MEMORY_MIB = 1792  # infra/iac/bench/main.tf


def limited_by(result: dict) -> str:
    """What gave out first in a run: the resource to optimise, or "none" when it passed with room left."""
    if result["server_oom_killed"] or (result["server_mem_peak_mib"] or 0) >= 0.95 * SERVER_MEMORY_MIB:
        return "memory"
    if result["gen_loop_delay_ms"] >= 250:
        return "generator (the generator ran late: the result says nothing about the server)"
    if result["core0_util"] >= 0.85:  # past ~85% busy, latency climbs from queueing
        return "cpu"
    if result["error_rate"] > 0:
        return "errors (connections refused or reset: descriptors, backlog or timeouts)"
    if not result["pass"]:
        return "latency (p95/p99 over the limit below 85% CPU: stalls)"
    return "none"


def run(name: str, vus: int, ramp: int, duration: int, gen: str, cpus, granian_flags=()) -> dict:
    start_all(name, cpus, granian_flags)
    samples, stop = [], threading.Event()
    threading.Thread(target=watch_memory, args=(samples, stop), daemon=True).start()
    cpu_before, hold = cat("/sys/fs/cgroup/cpu.stat"), {}
    # CPU 0 is measured over the hold only, when every user is active, like PR #71's figures
    timer = threading.Timer(ramp + 5, lambda: hold.update(start=core0(), at=time.monotonic()))
    timer.start()
    print(f"== {name}: {vus} users, {ramp}s ramp + {duration}s hold, {gen} generator"
          + (f", server capped at {cpus} CPU" if cpus else ""), flush=True)
    result = (c_gen if gen == "c" else python_gen)(vus, ramp, duration)
    timer.cancel()
    core_end = core0()
    stop.set()

    def usage_s(stat: str) -> float:
        for line in stat.splitlines():
            if line.startswith("usage_usec"):
                return int(line.split()[1]) / 1e6
        return 0.0

    delta = [b - a for a, b in zip(hold.get("start", core_end), core_end)]
    total = max(1, sum(delta))
    result.update(submission=name, gen=gen, cpus=cpus)
    result["server_cpu_s"] = round(usage_s(cat("/sys/fs/cgroup/cpu.stat")) - usage_s(cpu_before), 1)
    result["core0_util"] = round((total - delta[3] - delta[4]) / total, 3)  # hold; all but idle and iowait
    result["core0_softirq"] = round(delta[6] / total, 3)  # of which network receive processing
    peak = cat("/sys/fs/cgroup/memory.peak").strip()
    result["server_mem_peak_mib"] = round(int(peak) / 2**20, 1) if peak.isdigit() else None
    result["server_oom_killed"] = sh("docker", "inspect", "-f", "{{.State.OOMKilled}}", SERVER,
                                     capture=True).stdout.strip() == "true"
    result["server_samples"] = samples[-2:]
    if result["server_oom_killed"] or not result["pass"]:
        result["pass"] = False
        print(sh("docker", "logs", "--tail", "15", SERVER, check=False, capture=True).stderr, file=sys.stderr)
    result["limited_by"] = limited_by(result)
    stop_all()
    WORK.mkdir(parents=True, exist_ok=True)
    with open(WORK / "results.jsonl", "a") as f:
        f.write(json.dumps(result) + "\n")
    print(json.dumps(result), flush=True)
    return result


def ladder(name: str, ramp: int, duration: int, gen: str, cpus, resolution: int = 250, start: int = 2500) -> int:
    """`start` users, doubling on a pass or halving on a fail to find the boundary, then bisect to `resolution`."""
    good, bad, vus, failure = 0, None, start, "?"

    def passes(users: int) -> bool:
        nonlocal failure
        result = run(name, users, ramp, duration, gen, cpus)
        if not result["pass"]:
            failure = result["limited_by"]
        return result["pass"]

    while bad is None:
        if passes(vus):
            good, vus = vus, vus * 2
        else:
            bad = vus
    while good == 0 and bad > resolution:  # failed at the start: halve until a pass
        vus = bad // 2 // resolution * resolution or resolution
        if passes(vus):
            good = vus
        elif vus == resolution:
            break
        else:
            bad = vus
    while bad - good > resolution:
        mid = (good + bad) // 2 // resolution * resolution
        if passes(mid):
            good = mid
        else:
            bad = mid
    print(f"== {name} limit: {good} users pass, {bad} fail; the last failure was limited by {failure}", flush=True)
    return good


def calibrate(ramp: int, duration: int, gen: str, cpus, resolution: int) -> None:
    """Each REFS entry's limit at this CPU cap, next to its droplet score; then this entry's."""
    rows = []
    for name, ref in REFS.items():
        if ref["droplet"] is None:
            continue
        limit = ladder(name, ramp, duration, gen, cpus, resolution, start=ref["droplet"])
        rows.append((name, ref["droplet"], limit))
    ratios = [limit / droplet for _, droplet, limit in rows]
    ours = ladder("ours", ramp, duration, gen, cpus, resolution, start=rows[0][2] or 2500)
    print(f"\n== calibration at --cpus {cpus or 'uncapped'} ({ramp}s ramp + {duration}s hold, {gen} generator)")
    for name, droplet, limit in rows:
        print(f"   {name:12} PR #{REFS[name]['pr']:<3} droplet {droplet:6}  bench {limit:6}  ratio {limit / droplet:.2f}")
    mean = sum(ratios) / len(ratios)
    print(f"   ours                          bench {ours:6}  predicted droplet ~{round(ours / mean, -2):.0f}"
          f" (ratios {min(ratios):.2f}-{max(ratios):.2f})")
    with open(WORK / "calibration.jsonl", "a") as f:
        f.write(json.dumps({"cpus": cpus, "ramp": ramp, "duration": duration, "gen": gen, "refs": rows,
                            "ours": ours, "predicted": round(ours / mean)}) + "\n")


def memory_snapshot() -> dict:
    """The server container's memory: cgroup totals plus every process's RSS."""
    stat = dict(line.split() for line in cat("/sys/fs/cgroup/memory.stat").splitlines())
    mib = lambda b: round(int(b) / 2**20, 1)
    rss = sh("docker", "exec", SERVER, "sh", "-c", "cat /proc/[0-9]*/status", check=False, capture=True).stdout
    procs, name = {}, None
    for line in rss.splitlines():
        if line.startswith("Name:"):
            name = line.split()[1]
        elif line.startswith("VmRSS:") and name:
            procs[name] = procs.get(name, 0) + int(line.split()[1]) * 1024
    return {"current": mib(cat("/sys/fs/cgroup/memory.current").strip()), "anon": mib(stat["anon"]),
            "sock": mib(stat["sock"]), "kernel": mib(stat.get("kernel", 0)), "file": mib(stat["file"]),
            "rss": {k: mib(v) for k, v in procs.items() if v > 2**20}}


def memory(name: str, steps: str, request: str, granian_flags) -> None:
    """Idle keep-alive connections in steps; prints memory at each step and the per-connection slope."""
    start_all(name, None, granian_flags)
    time.sleep(2)
    rows = [{"held": 0, **memory_snapshot()}]
    print(json.dumps(rows[0]), flush=True)
    gen = subprocess.Popen(["docker", "exec", "-i", GENERATORS[0], "python", "idleconns.py", "--host", SERVER_IP,
                            "--steps", steps, "--request", request],
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        for line in gen.stdout:
            step = json.loads(line)
            if "error" in step:
                raise SystemExit(line)
            time.sleep(3)  # let the server settle
            rows.append({**step, **memory_snapshot()})
            print(json.dumps(rows[-1]), flush=True)
            gen.stdin.write("\n")
            gen.stdin.flush()
    finally:
        gen.kill()
        stop_all()
    first, last = rows[0], rows[-1]
    if last["held"]:
        per = lambda key: round((last[key] - first[key]) * 1024 / last["held"], 2)
        print(f"== per connection ({request}, {' '.join(granian_flags) or 'default flags'}): "
              f"{per('current')} KiB total, {per('anon')} KiB anon, {per('sock')} KiB sock, "
              f"{per('kernel')} KiB kernel", flush=True)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["run", "ladder", "calibrate", "memory"])
    p.add_argument("vus", type=int, nargs="?", default=2500, help="users (ladder: where to start)")
    p.add_argument("--submission", default="ours", choices=["ours", *REFS])
    p.add_argument("--ramp", type=int, default=60)
    p.add_argument("--duration", type=int, default=None, help="hold in seconds (300; calibrate: 60)")
    p.add_argument("--cpus", type=float, default=None, help="cap the server's CPU time (Docker --cpus)")
    p.add_argument("--gen", choices=["c", "python"], default="c", help="load generator")
    p.add_argument("--resolution", type=int, default=None, help="ladder step (250; calibrate: 1000)")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--steps", default="5000,10000,20000,35000", help="memory: idle connection counts")
    p.add_argument("--request", default="feed", help="memory: feed | health | none, sent once per connection")
    p.add_argument("--granian", action="append", default=[], help="extra Granian flag (repeatable)")
    args = p.parse_args()
    if sh("docker", "info", check=False, capture=True).returncode != 0:
        raise SystemExit("docker is not reachable: run `just vm start` (or start your own Docker) first")
    quick = args.mode == "calibrate"
    duration = args.duration or (60 if quick else 300)
    resolution = args.resolution or (1000 if quick else 250)
    if not args.no_build:
        build_gen_image()
        for name in (["ours", *REFS] if quick else [args.submission]):
            build_server_image(name)
    if args.mode == "run":
        run(args.submission, args.vus, args.ramp, duration, args.gen, args.cpus, args.granian)
    elif args.mode == "ladder":
        ladder(args.submission, args.ramp, duration, args.gen, args.cpus, resolution, start=args.vus)
    elif args.mode == "calibrate":
        calibrate(args.ramp, duration, args.gen, args.cpus, resolution)
    else:
        memory(args.submission, args.steps, args.request, args.granian)


if __name__ == "__main__":
    main()
