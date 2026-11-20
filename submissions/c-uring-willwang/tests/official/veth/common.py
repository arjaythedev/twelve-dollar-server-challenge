"""Small resource and private descriptor helpers for the optional native-veth benchmark."""

import array
import os
from pathlib import Path
import signal
import socket
import subprocess
import time


def run(*cmd, timeout=15, check=True, **kw):
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout, **kw)
    if check and result.returncode:
        raise RuntimeError(
            f"{cmd[:2]} returned {result.returncode}: {result.stderr.strip()} {result.stdout.strip()}"
        )
    return result.stdout.strip()


def docker(*cmd, **kw):
    # Explicitly select the local rootful daemon even if the caller has a remote context.
    return run("docker", "--host", "unix:///var/run/docker.sock", *cmd, **kw)


def validate_cpus(manager, server, groups):
    cpus = [server, *(c for group in groups for c in group)]
    if (
        not groups
        or any(not group for group in groups)
        or manager < 0
        or manager == server
        or min(cpus) < 0
        or len(set(cpus)) != len(cpus)
    ):
        raise ValueError("use nonempty disjoint client groups and keep the server CPU exclusive")
    os.sched_setaffinity(0, {manager})


def wait_path(path, note):
    deadline = time.monotonic() + 5
    while not path.exists():
        if time.monotonic() > deadline:
            raise RuntimeError(note())
        time.sleep(0.02)


def receive_namespace_fd(path, operation):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        peer.sendall(operation)
        _, anc, _, _ = peer.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        fds = array.array("i")
        for level, kind, data in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds.frombytes(data)
        if len(fds) != 1:
            raise ValueError("expected one descriptor from private namespace")
        return fds[0]


def tcp_socket(path):
    return receive_namespace_fd(path, b"\x00")


def subnet_request(path, cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(3)
        peer.connect(str(path))
        peer.sendall(cmd)
        return peer.recv(1024).decode().strip()


def stop(p):
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=3)


def collect_dir(base, key):
    out = base / key
    out.mkdir()
    out.chmod(0o777)
    return out


def group_of(pid):
    group = Path(f"/proc/{pid}/cgroup").read_text().strip().split("::")[-1]
    return Path("/sys/fs/cgroup") / group.lstrip("/")


def group_counts(group):
    r = {}
    try:
        for line in (group / "cpu.stat").read_text().splitlines():
            k, v = line.split()
            r[k] = int(v)
        for key in ("memory.current", "memory.peak", "memory.swap.current"):
            r[key] = int((group / key).read_text())
        for key in ("memory.stat", "memory.events"):
            for line in (group / key).read_text().splitlines():
                k, v = line.split()
                if key == "memory.events":
                    r[f"memory.events.{k}"] = int(v)
                elif k in ("sock", "slab", "kernel", "anon", "file"):
                    r[k] = int(v)
    except FileNotFoundError:
        # A scope can disappear between the CPU and memory snapshots at process exit.
        pass
    return r


def core_counts(cpus):
    out = {}
    want = {f"cpu{n}" for n in cpus}
    for line in Path("/proc/stat").read_text().splitlines():
        f = line.split()
        if f[0] not in want:
            continue
        out[f[0]] = {
            k: int(v)
            for k, v in zip(("user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal"), f[1:])
        }
    for line in Path("/proc/softirqs").read_text().splitlines():
        f = line.split()
        if f and f[0] in ("NET_RX:", "NET_TX:"):
            for cpu in cpus:
                out[f"cpu{cpu}"][f[0][:-1]] = int(f[cpu + 1])
    for n, line in enumerate(Path("/proc/net/softnet_stat").read_text().splitlines()):
        if n in cpus:
            for k, v in zip(("processed", "drop", "time_squeeze"), line.split()[:3]):
                out[f"cpu{n}"][k] = int(v, 16)
    return out


def tcp_sockstat(provider):
    data = subnet_request(provider, b"S").split()
    return {k: int(v) for k, v in zip(data[1::2], data[2::2])}


def wait_until(check, timeout, error):
    deadline = time.monotonic() + timeout
    while not check():
        if time.monotonic() > deadline:
            raise RuntimeError(error())
        time.sleep(0.04)
