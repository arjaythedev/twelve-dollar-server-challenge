"""Run the actual XDP executable on the harness's disposable server veth."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import time

from common import docker, group_of
from network import BASE_IP, HERE, STACK_IP
from sources import materialize


def runtime_libraries(*executables):
    """Mount just the shared libraries of these locally built, trusted executables."""
    paths = set()
    for executable in executables:
        output = subprocess.check_output(["ldd", str(executable)], text=True)
        for line in output.splitlines():
            if "=> not found" in line:
                raise RuntimeError(line.strip())
            match = re.search(r"(?:=>\s+|^\s*)(/\S+)\s+\(", line)
            if match:
                paths.add(Path(match[1]))
    return sorted(paths)


def mount(source, destination, *, writable=False):
    return ["--mount", f"type=bind,src={Path(source).resolve()},dst={destination}" +
            ("" if writable else ",readonly")]


def bootstrap(work):
    source = HERE / "xdp-start.c"
    executable = work / "xdp-start"
    if not executable.exists() or executable.stat().st_mtime_ns < source.stat().st_mtime_ns:
        with tempfile.TemporaryDirectory(dir=work) as stage:
            tmp = Path(stage) / "xdp-start"
            subprocess.run(["cc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-lcrypto", "-o", str(tmp)], check=True)
            os.replace(tmp, executable)
    return executable


def diagnostics(container, log):
    logs = subprocess.run(["docker", "--host", "unix:///var/run/docker.sock", "logs", container],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    return str(log.read_text()[-2600:]) + logs + docker("inspect", "--format", "{{json .State}}", container)


def verified_budget(pid, memory_bytes, cpu):
    # Docker's scope can become visible before systemd has written both v2
    # limits. The scorer starts only after the actual resource files agree.
    deadline = time.monotonic() + 1.5
    while True:
        group = group_of(pid)
        memory = (group / "memory.max").read_text().strip()
        swap = (group / "memory.swap.max").read_text().strip()
        cpus = sorted(os.sched_getaffinity(pid))
        if memory == str(memory_bytes) and swap == "0" and cpus == [cpu]:
            return group, {"cgroup": str(group), "memory.max": memory,
                           "memory.swap.max": swap, "cpus": cpus}
        if time.monotonic() >= deadline:
            raise RuntimeError(f"XDP resource budget at {group} has memory.max={memory}, "
                               f"memory.swap.max={swap}, cpus={cpus} "
                               f"(expected memory.max={memory_bytes}, memory.swap.max=0, cpus={[cpu]})")
        time.sleep(.025)


@contextmanager
def application(args, link, db, name, secret, source=None, *, launcher=None, readonly_mounts=()):
    from lab import PORT, digest, health

    if os.getuid() == 0 or os.getgid() == 0:
        raise ValueError("Run the private experiment as an ordinary user")
    if not (args.out / "client-wire-checksums.json").is_file():
        materialize(link)
    target = BASE_IP if args.shared_ip else STACK_IP
    mode = "xdp-skb" if getattr(args, "xdp_attachment", "auto") == "skb" else "xdp"
    options = ["--interface", "bench0", "--address", target + "/24", "--mode", mode]
    if args.shared_ip:
        options.append("--shared-ip")
    if getattr(args, "rx_cpu_stats", False):
        options.append("--rx-cpu-stats")
    for field, flag in (
        ("xdp_rx_frames", "--xdp-rx-frames"),
        ("xdp_tx_frames", "--xdp-tx-frames"),
        ("packet_tx_frames", "--packet-tx-frames"),
    ):
        depth = getattr(args, field, None)
        if depth:
            options.extend([flag, str(depth)])
    if getattr(args, "packet_gso", False):
        options.append("--packet-gso")
    # Linux 6.8 charges XDP UMEM against this UID's total locked pages. The
    # setup limit also accommodates a preceding, still-draining process.
    memlock = 64 * 2**20
    start = bootstrap(args.work)
    out = args.out / name
    out.mkdir()
    log = out / "server.log"
    log.touch(mode=0o600)
    mounts = mount(start, "/bootstrap") + mount(launcher or args.binary, "/server")
    for local, target_path in readonly_mounts:
        mounts += mount(local, target_path)
    mounts += mount(db.parent, "/data", writable=True) + mount(out, "/result", writable=True)
    executables = (start, args.binary, launcher) if launcher else (start, args.binary)
    for library in runtime_libraries(*executables):
        mounts += mount(library, str(library))
    expected = "-"
    if source:
        mounts += mount(source, "/seed-source")
        expected = digest(source)
    caps = []
    for capability in ("SETUID", "SETGID", "NET_RAW", "NET_ADMIN", "BPF"):
        caps.extend(["--cap-add", capability])
    cid = docker("run", "-d", *link.base("xdp-service"),
                 "--network", "container:" + link.server_ids[0], *caps,
                 "--cpuset-cpus", str(args.server_cpu), "--pids-limit", "8",
                 "--memory", f"{args.memory_mib}m", "--memory-swap", f"{args.memory_mib}m",
                 "--ulimit", "nofile=65535:65535", "--ulimit", f"memlock={memlock}:{memlock}",
                 "--env", f"SQLITE_PATH=/data/{db.name}", "--env", f"JWT_SECRET={secret}",
                 "--env", "HOST=0.0.0.0", "--env", f"PORT={PORT}", *mounts,
                 "--entrypoint", "/bootstrap", link.image, str(os.getuid()), str(os.getgid()),
                 str(link.host_inode), expected, "/server", *options)
    pidfd = None
    try:
        pid = int(docker("inspect", "--format", "{{.State.Pid}}", cid))
        deadline = time.monotonic() + 18
        while time.monotonic() < deadline:
            content = log.read_text()
            if "bypass ready mode=AF_XDP/" in content and health(link, target):
                break
            if not pid or not Path(f"/proc/{pid}").is_dir():
                raise RuntimeError("XDP startup failed: " + diagnostics(cid, log))
            time.sleep(.03)
        else:
            os.kill(pid, signal.SIGUSR1)
            time.sleep(.1)
            raise RuntimeError("XDP HTTP health timed out: " + diagnostics(cid, log))
        status = dict(line.split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines()
                      if ":" in line)
        if (int(status["Uid"].split()[0]) != os.getuid() or int(status["NoNewPrivs"]) != 1 or
            any(int(status[k], 16) for k in ("CapInh", "CapPrm", "CapEff", "CapAmb"))):
            raise RuntimeError("XDP service retained startup privileges")
        group, budget = verified_budget(pid, args.memory_mib * 2**20, args.server_cpu)
        ready = next(s for s in log.read_text().splitlines() if s.startswith("bypass ready mode="))
        runtime = {"pid": pid, "budget": budget, "memlock_limit_bytes": memlock, "link": {"driver": "veth", "mtu": 1500, "address": target,
                   "transport": ready, "effective_caps": 0, "uid": os.getuid()}, "container": cid}
        (out / "runtime.json").write_text(json.dumps(runtime, indent=2) + "\n")
        pidfd = os.pidfd_open(pid)
        yield target, pid, group, None, out
    finally:
        if pidfd is not None:
            try:
                if args.action == "score":
                    waiter = select.poll()
                    waiter.register(pidfd, select.POLLIN)
                    signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                    waiter.poll(4000)
            except ProcessLookupError:
                pass
            finally:
                os.close(pidfd)
        docker("rm", "-f", cid, check=False)
