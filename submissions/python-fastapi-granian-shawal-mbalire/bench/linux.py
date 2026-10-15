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
    just bench linux 20000                     # one run: ramp (users / 2000 s, 20-60 s) + 120 s hold
    just bench linux 20000 --duration 300      # the challenge's 5-minute hold
    just bench linux 20000 --cpus 0.5          # with the server's CPU capped
    just bench linux 11000 --submission cknutson12   # another entry (see REFS)
    just bench ladder --cpus 0.5               # the scoring ladder: 2,500 doubling, then narrow to 250
    just bench calibrate --cpus 0.5            # every REFS entry's limit next to its droplet score (see above)
    just bench memory                          # server memory per idle keep-alive connection
    just bench linux 20000 --gen python        # bench/loadgen.py instead of PR #14's C generator
    just bench memory --granian=--http1-buffer-size=8192   # extra Granian flags (this entry only)
    just bench linux 40000 --duration 120 --repeat 3 --profile   # perf on CPU 0 in each hold, then mean (min-max)
    just bench linux 40000 --duration 120 --profile python       # adds Python function names (inflates Python)

A SCHED_IDLE busy loop on CPU 7 keeps the Mac clocking the VM at full speed (see start_clock_keeper).
Four generator addresses give about 256k connections, so runs can pass 64k users; the server still
has the droplet's 65,535-descriptor limit and retires connections near it.
Stdlib only and Python 3.9 compatible, so the system python3 can run it.
"""

import argparse
import contextlib
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

import profiling

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
# CPU 7 also runs a SCHED_IDLE busy loop for the whole run (see start_clock_keeper).
CLOCK_KEEPER, CLOCK_KEEPER_CPU = "twelve-clock-keeper", "7"
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
SKIP = shutil.ignore_patterns(".venv", ".python", ".devtools", "build", "__pycache__", "bench", ".git", "node_modules")

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
# This entry, in layers: install.sh and build.sh run on just the files they read (install.sh, build.sh,
# infra/ops.py, requirements.txt), so a code change rebuilds only the last COPY (~100 s -> seconds).
# Still built with our own install.sh and build.sh, as on the box.
OURS_DOCKERFILE = """\
FROM ubuntu:24.04
COPY app/install.sh /app/install.sh
RUN bash /app/install.sh && useradd --create-home runner && chown -R runner /app
USER runner
COPY --chown=runner app/build.sh app/requirements.txt /app/
COPY --chown=runner app/infra/ops.py /app/infra/ops.py
RUN bash /app/build.sh
COPY --chown=runner feed.db /home/runner/feed.db
COPY --chown=runner app /app
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


# perf from the VM kernel's own linux-tools package, so it can read that kernel's samples and symbols.
PROFILER_IMAGE, PROFILER = "twelve-profiler", "twelve-profiler"
PROFILER_DOCKERFILE = """\
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \\
    linux-tools-{kernel} >/dev/null && ln -s /usr/lib/linux-tools/{kernel}/perf /usr/local/bin/perf
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
    shutil.rmtree(ctx / "app", ignore_errors=True)
    shutil.copytree(submission_dir(name), ctx / "app", ignore=SKIP)
    link_seed(ctx / "feed.db")
    (ctx / "Dockerfile").write_text(OURS_DOCKERFILE if name == "ours" else SERVER_DOCKERFILE)
    print(f"== building {name}", flush=True)
    sh("docker", "build", "-q", "-t", server_image(name), str(ctx), capture=True)


def link_seed(path: Path) -> None:
    """The 250 MB seed, hard-linked (copied if it can't be) and kept, so builds don't resend it."""
    seed = REPO / "seed" / "feed.db"
    if path.exists() and path.stat().st_size == seed.stat().st_size and path.stat().st_mtime == seed.stat().st_mtime:
        return
    path.unlink(missing_ok=True)
    try:
        os.link(seed, path)
    except OSError:
        shutil.copy2(seed, path)


def build_gen_image() -> None:
    ctx = WORK / "loadgen"
    ctx.mkdir(parents=True, exist_ok=True)
    original = ctx / "connection_load.original.c"  # downloaded once; checked against its pin every time
    if not original.exists():
        original.write_bytes(urllib.request.urlopen(CGEN_URL, timeout=30).read())
    source = original.read_bytes()
    if hashlib.sha256(source).hexdigest() != CGEN_SHA256:
        original.unlink()
        raise SystemExit("connection_load.c does not match its pinned SHA-256")
    text = source.decode()
    for old, new in CGEN_PATCH:
        if text.count(old) != 1:
            raise SystemExit(f"connection_load.c patch does not apply: {old!r}")
        text = text.replace(old, new)
    (ctx / "connection_load.c").write_text(text)
    for name in ("loadgen.py", "idleconns.py"):
        shutil.copy2(HERE / name, ctx / name)
    shutil.copy2(REPO / "seed" / "tokens.json", ctx / "tokens.json")
    (ctx / "Dockerfile").write_text(GEN_DOCKERFILE)
    sh("docker", "build", "-q", "-t", GEN_IMAGE, str(ctx), capture=True)


def build_profiler_image() -> None:
    kernel = sh("docker", "info", "--format", "{{.KernelVersion}}", capture=True).stdout.strip()
    ctx = WORK / "profiler"
    ctx.mkdir(parents=True, exist_ok=True)
    (ctx / "Dockerfile").write_text(PROFILER_DOCKERFILE.format(kernel=kernel))
    print(f"== building the profiler (perf for kernel {kernel})", flush=True)
    sh("docker", "build", "-q", "-t", PROFILER_IMAGE, str(ctx), capture=True)


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


def terraform_variables(name: str, cpus, granian_flags, server_env=()) -> list:
    """infra/iac/bench's variables for one run, as -var flags."""
    variables = {
        "docker_host": docker_host(), "server_image": server_image(name), "generator_image": GEN_IMAGE,
        "server_command": json.dumps(server_command(name, granian_flags)),
        "server_env": json.dumps(list(server_env)),
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


def start_clock_keeper() -> None:
    """A SCHED_IDLE busy loop on CPU 7, so the Mac runs the VM's cores at full clock for the whole run.

    SCHED_IDLE: the generator on CPU 7 always wins the CPU; the loop only fills its idle time. (With the
    loop on a CPU of its own and 3 generators, the generators fell 37-60 s behind at 140-160k users.)

    Apple Silicon clocks a cluster by its load: with CPU 0 at ~25-45% and the rest of the VM idle,
    the same server cost 144 µs per request (core0_util 44%); with this loop running, 68 µs (24%).
    Until 2026-10-15 a spinning lima-guestagent did this by accident, on and off (74-85 µs runs).
    A droplet's vCPU runs at a steady clock, which this models.
    """
    sh("docker", "run", "-d", "--rm", "--name", CLOCK_KEEPER, "--cpuset-cpus", CLOCK_KEEPER_CPU, GEN_IMAGE,
       "chrt", "-i", "0", "sh", "-c", "while :; do :; done", capture=True)


def start_all(name: str, cpus, granian_flags=(), server_env=()) -> None:
    """A fresh server (and database) and fresh generators for every run. Call stop_all() first."""
    start_clock_keeper()
    terraform("apply", "-input=false", "-auto-approve", *terraform_variables(name, cpus, granian_flags, server_env))
    steering = [(SERVER, SERVER_CPUMASK, GENERATORS_CPUMASK)]
    steering += [(generator, cpumask(cpu_set), cpumask(cpu_set))
                 for generator, cpu_set in zip(GENERATORS, GENERATOR_CPU_SETS)]
    threads = [threading.Thread(target=set_rps, args=arguments) for arguments in steering]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    wait_healthy()


# Samples the server's network namespace five times a second from outside it (CPU 6, not the server's
# CPU 0), cheaply: TcpExt ListenOverflows/ListenDrops count connections the kernel dropped because the
# accept queue was full (the client then retries its SYN after 1 s, 3 s, ...), and sockstat's TCP
# "inuse" counts the namespace's open sockets, about the server's descriptors. (Listing
# /proc/<pid>/fd every second instead cost a generator CPU enough to make it run 0.6 s late.)
SAMPLER, SAMPLER_CPU = "twelve-sampler", "6"
SAMPLER_SCRIPT = """\
import json, sys, time
pid = sys.argv[1]
while True:
    lines = [line.split() for line in open(f"/proc/{pid}/net/netstat") if line.startswith("TcpExt:")]
    values = dict(zip(lines[0][1:], map(int, lines[1][1:])))
    tcp = {}
    for line in open(f"/proc/{pid}/net/sockstat"):
        if line.startswith("TCP:"):
            fields = line.split()[1:]
            tcp = dict(zip(fields[::2], map(int, fields[1::2])))
    cpu0 = next(line.split()[1:9] for line in open("/proc/stat") if line.startswith("cpu0 "))
    print(json.dumps({"t": time.time(), "sockets": tcp.get("inuse", 0), "orphans": tcp.get("orphan", 0),
                      "time_wait": tcp.get("tw", 0), "overflows": values.get("ListenOverflows", 0),
                      "drops": values.get("ListenDrops", 0), "cpu0": list(map(int, cpu0))}), flush=True)
    time.sleep(0.2)
"""


def start_sampler() -> None:
    pid = sh("docker", "inspect", "-f", "{{.State.Pid}}", SERVER, capture=True).stdout.strip()
    sh("docker", "rm", "-f", SAMPLER, check=False, capture=True)
    sh("docker", "run", "-d", "--name", SAMPLER, "--pid", "host",
       "--cpuset-cpus", SAMPLER_CPU, GEN_IMAGE, "python", "-c", SAMPLER_SCRIPT, pid, capture=True)


def sampler_summary(hold_from: float, rows_file: Path) -> dict:
    """Listen-queue drops (whole run and hold only), peak open sockets, seconds near the descriptor limit.

    Every sample is kept in `rows_file` (JSON lines) for time series: bench/samples.py summarizes one.
    """
    out = sh("docker", "logs", SAMPLER, check=False, capture=True).stdout
    rows = [json.loads(line) for line in out.splitlines() if line.startswith("{")]
    sh("docker", "rm", "-f", SAMPLER, check=False, capture=True)
    if not rows:
        return {}
    rows_file.parent.mkdir(parents=True, exist_ok=True)
    rows_file.write_text("".join(json.dumps({**row, "hold_from": hold_from}) + "\n" for row in rows))
    hold = [row for row in rows if row["t"] >= hold_from] or rows[-1:]
    return {
        "listen_overflows": rows[-1]["overflows"] - rows[0]["overflows"],
        "listen_overflows_hold": hold[-1]["overflows"] - hold[0]["overflows"],
        "listen_drops": rows[-1]["drops"] - rows[0]["drops"],
        "peak_sockets": max(row["sockets"] for row in rows),
        "seconds_near_descriptor_limit": round(sum(row["sockets"] >= 65_535 - 512 for row in rows) * 0.2, 1),
        "samples_file": str(rows_file),
    }


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
    sh("docker", "rm", "-f", "twelve-loadgen", PROFILER, CLOCK_KEEPER, SAMPLER, check=False, capture=True)
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


def vm_proc(command: str) -> str:
    """Runs `command` in a throwaway container that sees the VM's processes (read only)."""
    return sh("docker", "run", "--rm", "--pid", "host", GEN_IMAGE, "sh", "-c", command,
              check=False, capture=True).stdout


def vm_busy(seconds: float = 3.0) -> float:
    """Share of the VM's CPUs in use over `seconds`, from /proc/stat."""
    out = vm_proc(f"head -1 /proc/stat; sleep {seconds}; head -1 /proc/stat").split("\n")
    first, second = ([int(x) for x in line.split()[1:9]] for line in out[:2])
    delta = [b - a for a, b in zip(first, second)]
    return 1 - (delta[3] + delta[4]) / max(1, sum(delta))


def guest_agent_cpu_s() -> float:
    """CPU seconds used so far by Colima's lima-guestagent (it has spun at 100% of a CPU before)."""
    total = 0
    for line in vm_proc("cat /proc/[0-9]*/stat 2>/dev/null").splitlines():
        if "(lima-guestagent)" in line:
            fields = line.rsplit(")", 1)[1].split()
            total += int(fields[11]) + int(fields[12])  # utime, stime
    return total / 100


COLIMA_PROFILE = "bench"  # `just vm start`


@contextlib.contextmanager
def guest_agent_stopped():
    """Stops Colima's lima-guestagent for the bench, and starts it again afterwards, whatever happens.

    It looks for listening ports to forward to the Mac by scanning the VM's sockets; with 40k
    connections open it spins at 100% of a CPU for the whole run (181 CPU-seconds in one 3-minute
    run) and keeps spinning after. Docker doesn't need it: the socket comes over Colima's SSH.
    """
    colima = shutil.which("colima") is not None and f".colima/{COLIMA_PROFILE}/" in docker_host()
    if colima:
        sh("colima", "ssh", "-p", COLIMA_PROFILE, "--", "sudo", "systemctl", "stop", "lima-guestagent")
    try:
        yield
    finally:
        if colima:
            sh("colima", "ssh", "-p", COLIMA_PROFILE, "--", "sudo", "systemctl", "start", "lima-guestagent",
               check=False)


def require_idle_vm() -> None:
    """Refuses to start a run while something else keeps the VM busy: it would skew every number."""
    busy = vm_busy()
    if busy > 0.05:
        print(vm_proc("top -b -n 1 | sed -n 7,12p"), file=sys.stderr)
        raise SystemExit(f"the VM is {busy:.0%} busy before the run. If lima-guestagent is spinning: "
                         "colima ssh -p bench -- sudo systemctl restart lima-guestagent")


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


# --- profiling --------------------------------------------------------------------------------------

PROFILE_SCRIPT = """\
set -e
mkdir -p /out
perf record -q -F {hz} -C 0 -g -o /out/perf.data -- sleep {seconds} 2>/dev/null
perf script -i /out/perf.data > /out/perf.script 2>/dev/null
"""
# Python's perf trampolines (PYTHONPERFSUPPORT=1) name Python functions in the stacks, but they add a
# frame to every Python call, which inflates call-heavy Python (routing showed 5.7% profiled, ~3% timed).
# hops: perf as in layers, plus the server's own per-hop spans (PROFILE_HOPS_OUT: the timed railway
# runner and adapters/span_recorder.py; costs ~2.5-3.3 µs per read request in-process).
SERVER_HOPS_FILE = "/home/runner/hops.json"
PROFILE_MODES = {"layers": (), "python": ("PYTHONPERFSUPPORT=1",), "hops": ("PROFILE_HOPS_OUT=" + SERVER_HOPS_FILE,)}


def profile(seconds: int, out: Path) -> None:
    """perf on CPU 0 for `seconds` (the kernel, Granian, SQLite and Python together), folded into out/.

    Ubuntu 24.04 builds python3.12 and libsqlite3 with frame pointers, so -g unwinds through them. The
    VM has no PMU, so perf samples on a timer (cpu-clock), idle included; bench/profiling.py drops idle.
    """
    sh("docker", "rm", "-f", PROFILER, check=False, capture=True)
    sh("docker", "run", "--name", PROFILER, "--privileged", "--pid", "host", "--cpuset-cpus", "6", PROFILER_IMAGE,
       "sh", "-c", PROFILE_SCRIPT.format(hz=999, seconds=seconds), capture=True)
    out.mkdir(parents=True, exist_ok=True)
    script = out / "perf.script"
    sh("docker", "cp", f"{PROFILER}:/out/perf.script", str(script), capture=True)
    sh("docker", "rm", "-f", PROFILER, check=False, capture=True)
    profiling.write_folded(profiling.fold_script(script), out / "stacks.folded")
    script.unlink()  # ~100 MB a minute; stacks.folded keeps every stack


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


def default_ramp(vus: int) -> int:
    """2,000 new users a second (the old fixed 60 s at 120k), 20-60 s."""
    return min(60, max(20, vus // 2000))


def run(name: str, vus: int, ramp: int, duration: int, gen: str, cpus, granian_flags=(), profile_mode=None,
        label=None) -> dict:
    """One run; with profile_mode ("layers" or "python"), perf samples CPU 0 during the hold."""
    ramp = ramp if ramp is not None else default_ramp(vus)
    stop_all()
    require_idle_vm()
    agent_before = guest_agent_cpu_s()
    start_all(name, cpus, granian_flags, PROFILE_MODES[profile_mode] if profile_mode else ())
    start_sampler()
    hold_from = time.time() + ramp
    samples, stop = [], threading.Event()
    threading.Thread(target=watch_memory, args=(samples, stop), daemon=True).start()
    cpu_before, hold = cat("/sys/fs/cgroup/cpu.stat"), {}
    # CPU 0 is measured over the hold only, when every user is active, like PR #71's figures
    timer = threading.Timer(ramp + 5, lambda: hold.update(start=core0(), at=time.monotonic()))
    timer.start()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    profile_dir = WORK / "profiles" / stamp
    profiler = None
    if profile_mode:  # the middle of the hold, clear of the ramp and the ramp-down
        seconds = max(10, min(60, duration - 20))
        profiler = threading.Timer(ramp + 10, profile, args=(seconds, profile_dir))
        profiler.start()
    print(f"== {name}: {vus} users, {ramp}s ramp + {duration}s hold, {gen} generator"
          + (f", server capped at {cpus} CPU" if cpus else ""), flush=True)
    result = (c_gen if gen == "c" else python_gen)(vus, ramp, duration)
    timer.cancel()
    if profiler is not None:
        profiler.join()
    if profile_mode == "hops":  # before the server goes away; it flushes every 5 s
        profile_dir.mkdir(parents=True, exist_ok=True)
        sh("docker", "cp", f"{SERVER}:{SERVER_HOPS_FILE}", str(profile_dir / "hops.json"), check=False, capture=True)
    core_end = core0()
    stop.set()

    def usage_s(stat: str) -> float:
        for line in stat.splitlines():
            if line.startswith("usage_usec"):
                return int(line.split()[1]) / 1e6
        return 0.0

    delta = [b - a for a, b in zip(hold.get("start", core_end), core_end)]
    total = max(1, sum(delta))
    result.update(submission=name, gen=gen, cpus=cpus, ramp_s=ramp, finished_at=time.strftime("%Y-%m-%d %H:%M"))
    if label:
        result["label"] = label
    if profile_mode:  # perf (and the trampolines) cost CPU: compare profiled runs only with each other
        result["profiled"] = profile_mode
        result["profile_dir"] = str(profile_dir)
    result["server_cpu_s"] = round(usage_s(cat("/sys/fs/cgroup/cpu.stat")) - usage_s(cpu_before), 1)
    result["core0_util"] = round((total - delta[3] - delta[4]) / total, 3)  # hold; all but idle and iowait
    result["core0_softirq"] = round(delta[6] / total, 3)  # of which network receive processing
    peak = cat("/sys/fs/cgroup/memory.peak").strip()
    result["server_mem_peak_mib"] = round(int(peak) / 2**20, 1) if peak.isdigit() else None
    result["server_oom_killed"] = sh("docker", "inspect", "-f", "{{.State.OOMKilled}}", SERVER,
                                     capture=True).stdout.strip() == "true"
    result["server_samples"] = samples[-2:]
    result["guest_agent_cpu_s"] = round(guest_agent_cpu_s() - agent_before, 1)
    result["hold_s"] = duration
    result.update(sampler_summary(hold_from, WORK / "samples" / (stamp + ".jsonl")))
    logs = sh("docker", "logs", SERVER, check=False, capture=True)
    result["server_log_emfile"] = (logs.stdout + logs.stderr).count("Too many open files")
    if result["server_oom_killed"] or not result["pass"]:
        result["pass"] = False
        print(sh("docker", "logs", "--tail", "15", SERVER, check=False, capture=True).stderr, file=sys.stderr)
    result["limited_by"] = limited_by(result)
    stop_all()
    if profile_mode and (profile_dir / "stacks.folded").exists():  # before results.jsonl, so it keeps it
        result["profile"] = profiling.save_summary(profile_dir, profiling.read_folded(profile_dir / "stacks.folded"))
    if (profile_dir / "hops.json").exists():
        result["hops"] = json.loads((profile_dir / "hops.json").read_text())
    WORK.mkdir(parents=True, exist_ok=True)
    with open(WORK / "results.jsonl", "a") as f:
        f.write(json.dumps(result) + "\n")
    print(json.dumps(result), flush=True)
    if profile_mode and (profile_dir / "stacks.folded").exists():
        print(f"== profile: {profile_dir} (stacks.folded opens at https://www.speedscope.app)", flush=True)
    return result


def repeated(times: int, *args, **kwargs) -> None:
    """`times` runs in a row, then every number as mean (min-max); saved under build/linuxbench/aggregates/."""
    results = [run(*args, **kwargs) for _ in range(times)]
    summaries = [result["profile"] for result in results if "profile" in result]
    agg = profiling.aggregate(summaries, results)
    agg.update(profiling.aggregate_hops([result["hops"] for result in results if "hops" in result]))
    agg["profile_dirs"] = [result.get("profile_dir") for result in results]
    out = WORK / "aggregates"
    out.mkdir(parents=True, exist_ok=True)
    path = out / (time.strftime("%Y%m%d-%H%M%S") + ".json")
    path.write_text(json.dumps(agg, indent=1) + "\n")
    profiling.print_aggregate(agg)
    print(f"== aggregate: {path}", flush=True)


def ladder(name: str, ramp: int, duration: int, gen: str, cpus, resolution: int = 250, start: int = 2500,
           upper=None) -> int:
    """`start` users, doubling on a pass or halving on a fail to find the boundary, then bisect to `resolution`.

    With `upper` (a load already known to fail), a pass at `start` goes straight to bisecting, which saves
    the doubling run (e.g. 240k users) at high loads.
    """
    good, bad, vus, failure = 0, upper, start, "?"

    def passes(users: int) -> bool:
        nonlocal failure
        result = run(name, users, ramp, duration, gen, cpus)
        if not result["pass"]:
            failure = result["limited_by"]
        return result["pass"]

    if bad is not None:
        if passes(vus):
            good = vus
        else:
            bad = vus
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
    print(f"\n== calibration at --cpus {cpus or 'uncapped'} ({ramp or 'auto'}s ramp + {duration}s hold, {gen} generator)")
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
    stop_all()
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


# --- A/B -------------------------------------------------------------------------------------------

AB_PREFIX = "twelve-ab-"  # just bench snapshot NAME -> image twelve-ab-NAME


def snapshot(name: str) -> None:
    """Builds the working tree into image twelve-ab-NAME, a variant for `ab`."""
    build_server_image("ours")
    sh("docker", "tag", server_image("ours"), AB_PREFIX + name)
    print(f"== snapshot: {AB_PREFIX + name} (this working tree)", flush=True)


def per_request_us(result: dict) -> float:
    return round(result["server_cpu_s"] / result["requests"] * 1e6, 1) if result.get("requests") else 0.0


def ab(vus: int, variants: list, rounds: int, ramp, duration: int, gen: str, granian_flags) -> None:
    """Alternates the snapshot images run by run (A B A B ...), so the Mac's drift hits both alike."""
    for variant in variants:
        if sh("docker", "image", "inspect", AB_PREFIX + variant, check=False, capture=True).returncode != 0:
            raise SystemExit(f"no image {AB_PREFIX + variant}: run `just bench snapshot {variant}` first")
    by_variant = {variant: [] for variant in variants}
    for _ in range(rounds):
        for variant in variants:
            sh("docker", "tag", AB_PREFIX + variant, server_image("ours"))
            by_variant[variant].append(run("ours", vus, ramp, duration, gen, None, granian_flags, label=variant))
    columns = [("µs/req", per_request_us), ("core0", lambda r: f"{r['core0_util']:.0%}"),
               ("p95 ms", lambda r: round(r["p95_ms"])), ("p99 ms", lambda r: round(r["p99_ms"])),
               ("connects", lambda r: r["connects"]), ("dropped", lambda r: r.get("listen_overflows", "-")),
               ("pass", lambda r: "yes" if r["pass"] else "no")]
    print(f"\n== A/B at {vus} users, {rounds} rounds (runs listed in order)")
    for name, value in columns:
        print(f"   {name:9}" + "".join(f"  {variant}: " + " / ".join(str(value(r)) for r in results)
                                      for variant, results in by_variant.items()))
    out = WORK / "ab"
    out.mkdir(parents=True, exist_ok=True)
    path = out / (time.strftime("%Y%m%d-%H%M%S") + ".json")
    path.write_text(json.dumps({"vus": vus, "rounds": rounds, "runs": by_variant}, indent=1) + "\n")
    print(f"== saved {path}; {server_image('ours')} is now {AB_PREFIX + variants[-1]}", flush=True)


def results(count: int) -> None:
    """The last `count` runs from build/linuxbench/results.jsonl, one line each."""
    path = WORK / "results.jsonl"
    if not path.exists():
        raise SystemExit("no runs yet")
    rows = [json.loads(line) for line in path.read_text().splitlines()[-count:]]
    print(f"{'finished':16} {'entry':14} {'users':>7} {'hold':>5} {'pass':>4} {'p95 ms':>8} {'p99 ms':>8} "
          f"{'µs/req':>7} {'core0':>6} {'connects':>9} {'dropped':>8} {'peak MiB':>9}")
    for r in rows:
        print(f"{r.get('finished_at', '-'):16} {r.get('label') or r['submission']:14.14} {r['vus']:>7} "
              f"{r.get('hold_s', '-'):>5} {'yes' if r['pass'] else 'no':>4} {r['p95_ms']:>8.0f} {r['p99_ms']:>8.0f} "
              f"{per_request_us(r):>7} {r['core0_util']:>6.0%} {r['connects']:>9} "
              f"{r.get('listen_overflows', '-'):>8} {r.get('server_mem_peak_mib') or '-':>9}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["run", "ladder", "calibrate", "memory", "snapshot", "ab", "results"])
    p.add_argument("targets", nargs="*", help="run/ladder: users (ladder: where to start); snapshot: NAME; "
                   "ab: USERS A B; results: how many (10)")
    p.add_argument("--rounds", type=int, default=3, help="ab: runs per variant")
    p.add_argument("--submission", default="ours", choices=["ours", *REFS])
    p.add_argument("--ramp", type=int, default=None, help="seconds (default: users / 2000, 20-60)")
    p.add_argument("--duration", type=int, default=None, help="hold in seconds (120; calibrate: 60)")
    p.add_argument("--cpus", type=float, default=None, help="cap the server's CPU time (Docker --cpus)")
    p.add_argument("--gen", choices=["c", "python"], default="c", help="load generator")
    p.add_argument("--resolution", type=int, default=None, help="ladder step (250; calibrate: 1000)")
    p.add_argument("--upper", type=int, default=None, help="ladder: a load known to fail; bisect below it")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--steps", default="5000,10000,20000,35000", help="memory: idle connection counts")
    p.add_argument("--request", default="feed", help="memory: feed | health | none, sent once per connection")
    p.add_argument("--granian", action="append", default=[], help="extra Granian flag (repeatable)")
    p.add_argument("--profile", nargs="?", const="layers", choices=sorted(PROFILE_MODES),
                   help="run: perf on CPU 0 during the hold; layers (default), python (adds function "
                        "names) or hops (adds the server's per-hop spans; ours only)")
    p.add_argument("--repeat", type=int, default=1, help="run: this many runs, then mean (min-max) of each number")
    args = p.parse_args()
    expected = {"snapshot": 1, "ab": 3}.get(args.mode)
    if expected is not None and len(args.targets) != expected:
        raise SystemExit({"snapshot": "usage: snapshot NAME", "ab": "usage: ab USERS A B"}[args.mode])
    args.vus = int(args.targets[0]) if args.targets and args.mode not in ("snapshot",) else 2500
    if args.mode == "results":
        results(int(args.targets[0]) if args.targets else 10)
        return
    if sh("docker", "info", check=False, capture=True).returncode != 0:
        raise SystemExit("docker is not reachable: run `just vm start` (or start your own Docker) first")
    quick = args.mode == "calibrate"
    duration = args.duration or (60 if quick else 120)
    resolution = args.resolution or (1000 if quick else 250)
    if args.mode == "snapshot":
        snapshot(args.targets[0])
        return
    if not args.no_build:
        build_gen_image()
        if args.profile:
            build_profiler_image()
        for name in [] if args.mode == "ab" else (["ours", *REFS] if quick else [args.submission]):
            build_server_image(name)
    with guest_agent_stopped():
        dispatch(args, duration, resolution)


def dispatch(args, duration: int, resolution: int) -> None:
    if args.mode == "run":
        repeated(args.repeat, args.submission, args.vus, args.ramp, duration, args.gen, args.cpus, args.granian,
                 args.profile)
    elif args.mode == "ladder":
        ladder(args.submission, args.ramp, duration, args.gen, args.cpus, resolution, start=args.vus, upper=args.upper)
    elif args.mode == "ab":
        ab(args.vus, args.targets[1:], args.rounds, args.ramp, duration, args.gen, args.granian)
    elif args.mode == "calibrate":
        calibrate(args.ramp, duration, args.gen, args.cpus, resolution)
    else:
        memory(args.submission, args.steps, args.request, args.granian)


if __name__ == "__main__":
    main()
