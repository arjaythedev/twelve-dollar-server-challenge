"""Apply the measured passive IPv4 TCP charge to exactly one verified transient slice."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import select
import struct
import sys
import time

MIB = 1024**2
APP = 1750 * MIB
GUEST = 1967 * MIB
PER_SOCKET = 2376
CADENCE_MS = 100
PAGE = os.sysconf("SC_PAGE_SIZE")
CG = Path("/sys/fs/cgroup")
ID_PATTERN = r"tr[0-9a-f]{7}"


def private_paths(marker):
    if not re.fullmatch(ID_PATTERN, marker):
        raise ValueError("unexpected private trial identity")
    unit = f"twelve-revisit-{marker}.service"
    sl = f"twelvepassive{marker}.slice"
    return sl, unit


def require(ok, detail):
    if not ok:
        raise RuntimeError(detail)


def sample_number(fd):
    return int(os.pread(fd, 512, 0).strip())


def pairs(text):
    return dict(zip(text[::2], text[1::2]))


def tcp_inuse(fd):
    raw = os.pread(fd, 4096, 0).decode()
    rows = [row.split()[1:] for row in raw.splitlines() if row.startswith("TCP:")]
    require(len(rows) == 1, "missing private IPv4 sockstat")
    return int(pairs(rows[0])["inuse"])


def active_opens(fd):
    tcp = [
        line.split()[1:] for line in os.pread(fd, 32768, 0).decode().splitlines() if line.startswith("Tcp:")
    ]
    require(len(tcp) == 2, "missing private namespace SNMP counters")
    return int(dict(zip(*tcp))["ActiveOpens"])


def pressure_totals(fd):
    result = {}
    for line in os.pread(fd, 4096, 0).decode().splitlines():
        fields = line.split()
        result[fields[0]] = int(dict(x.split("=", 1) for x in fields[1:])["total"])
    return result


def current_events(fd):
    return {k: int(v) for k, v in (line.split() for line in os.pread(fd, 4096, 0).decode().splitlines())}


def listeners(net, port):
    lines = (net / "tcp").read_text().splitlines()[1:]
    sockets = [line.split() for line in lines if line.split()[3] == "0A"]
    require(
        sockets and all(int(s[1].split(":")[1], 16) == port for s in sockets),
        "private namespace must contain only the selected IPv4 service listeners",
    )
    if (net / "tcp6").is_file():
        require(
            not any(line.split()[3] == "0A" for line in (net / "tcp6").read_text().splitlines()[1:]),
            "the measured passive cost applies only to IPv4 listeners",
        )
    return len(sockets)


def budget(app, guest, count):
    return min(app, max(0, guest - count * PER_SOCKET)) // PAGE * PAGE


def verified_fds(a):
    sl, unit = private_paths(a.marker)
    outer = CG / sl
    leaf = outer / unit
    proc = Path("/proc") / str(a.pid)
    target = f"/{sl}/{unit}"
    uid = int(os.environ["SUDO_UID"])
    gid = int(os.environ["SUDO_GID"])
    require(os.geteuid() == 0 and uid > 0 and gid > 0, "launch as the ordinary fixture user through sudo")
    require(
        outer.is_dir()
        and {d.name for d in outer.iterdir() if d.is_dir()} == {unit}
        and not (outer / "cgroup.procs").read_text().strip(),
        "slice is not exclusive to this service",
    )
    require(not any(d.is_dir() for d in leaf.iterdir()), "service unexpectedly owns other cgroups")
    require(
        (proc / "cgroup").read_text().strip() == "0::" + target
        and str(a.pid) in (leaf / "cgroup.procs").read_text().split(),
        "service PID disagrees with the private cgroup",
    )
    status = (proc / "status").read_text()
    ids = re.search(r"^Uid:\s+(.*)$", status, re.M)
    require(ids and set(map(int, ids[1].split())) == {uid}, "service does not belong to the invoking user")
    require(
        os.sched_getaffinity(0) == {a.cpu} and os.sched_getaffinity(a.pid) == {a.app_cpu},
        "CPU identity mismatch",
    )
    expected_net = (Path("/run/netns") / (a.marker + "s")).stat()
    actual_net = (proc / "ns/net").stat()
    require(
        (expected_net.st_dev, expected_net.st_ino) == (actual_net.st_dev, actual_net.st_ino)
        and expected_net.st_ino != Path("/proc/self/ns/net").stat().st_ino,
        "unexpected or host network namespace",
    )
    require(os.path.samefile(proc / "exe", a.exe), "actual service executable is not the requested ELF")
    require(
        (leaf / "memory.max").read_text().strip() == str(a.app)
        and (leaf / "memory.swap.max").read_text().strip() == "0",
        "original app cap or swap limit differs",
    )
    require((outer / "memory.max").read_text().strip() == "max", "outer slice was already limited")
    slot = int(Path("/sys/kernel/slab/TCP/slab_size").read_text())
    require(
        slot == 2368 and struct.calcsize("P") == 8,
        "SLUB geometry differs from the paired IPv4 charge calibration",
    )
    base = listeners(proc / "net", a.port)
    pidfd = os.pidfd_open(a.pid)
    od = os.open(outer, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    ld = os.open(leaf, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)

    def attr(directory, name, flags=os.O_RDONLY):
        return os.open(name, flags | os.O_NOFOLLOW, dir_fd=directory)

    handles = dict(
        max=attr(od, "memory.max", os.O_RDWR),
        current=attr(od, "memory.current"),
        events=attr(od, "memory.events"),
        swap=attr(od, "memory.swap.current"),
        app_max=attr(ld, "memory.max"),
        app_swap=attr(ld, "memory.swap.max"),
        pid=pidfd,
        app_current=attr(ld, "memory.current"),
        app_events=attr(ld, "memory.events"),
        parent_local=attr(od, "memory.events.local"),
        app_local=attr(ld, "memory.events.local"),
        parent_psi=attr(od, "memory.pressure"),
        app_psi=attr(ld, "memory.pressure"),
        memory_stat=attr(od, "memory.stat"),
        sock=os.open(proc / "net/sockstat", os.O_RDONLY),
        snmp=os.open(proc / "net/snmp", os.O_RDONLY),
    )
    os.close(od)
    os.close(ld)
    require(tcp_inuse(handles["sock"]) >= base, "namespace sockstat omitted the listeners")
    identity = dict(
        pid=a.pid,
        uid=uid,
        netns=actual_net.st_ino,
        slice=sl,
        unit=unit,
        group=target,
        outer_inode=outer.stat().st_ino,
        service_inode=leaf.stat().st_ino,
        listeners=base,
        initial_active_opens=active_opens(handles["snmp"]),
        slot_bytes=slot,
        objcg_bytes=8,
        charge_bytes=PER_SOCKET,
        kernel=os.uname().release,
        original_leaf_max=a.app,
        original_leaf_swap_max=0,
        upper_envelope=a.guest,
        manager_cpu=a.cpu,
        app_cpu=a.app_cpu,
        cadence_ms=a.cadence,
    )
    os.setgroups([])
    os.setgid(gid)
    os.setuid(uid)
    require(ctypes.CDLL(None, use_errno=True).prctl(38, 1, 0, 0, 0) == 0, "PR_SET_NO_NEW_PRIVS")
    status = Path("/proc/self/status").read_text()
    require(
        re.search(r"^CapEff:\s+0+$", status, re.M) and re.search(r"^NoNewPrivs:\s+1$", status, re.M),
        "manager did not drop effective capabilities",
    )
    identity.update(
        manager_uid=os.geteuid(),
        manager_caps="0",
        manager_no_new_privileges=True,
        manager_cgroup=Path("/proc/self/cgroup").read_text().strip(),
    )
    require(
        target not in identity["manager_cgroup"] and sl not in identity["manager_cgroup"],
        "manager entered the service budget",
    )
    return handles, identity


def emit(kind, **values):
    print(
        json.dumps(
            dict(kind=kind, unix_ns=time.time_ns(), mono_ns=time.monotonic_ns(), **values),
            separators=(",", ":"),
        ),
        flush=True,
    )


def finish_monitor(handles, reason, rows, last_events):
    try:
        events = current_events(handles["events"])
        local = current_events(handles["parent_local"])
        final_events_available = True
    except OSError:
        # systemd can remove the just-emptied cgroup as its main process exits.
        events = last_events
        local = {}
        final_events_available = False
    emit(
        "end",
        reason=reason,
        samples=rows,
        events=events,
        parent_events_local=local,
        final_events_available=final_events_available,
        manager_cpu_ns=time.process_time_ns(),
    )


def main(a):
    require(
        0 < a.app <= APP
        and a.app < a.guest <= GUEST
        and a.app % PAGE == 0
        and 20 <= a.cadence <= 1000
        and 1 <= a.port <= 65535,
        "invalid envelope or cadence",
    )
    h, info = verified_fds(a)
    poller = select.poll()
    poller.register(h["pid"], select.POLLIN)
    poller.register(0, select.POLLIN | select.POLLHUP)
    emit("identity", **info)
    last_cap = None
    previous = None
    last_at = None
    clock = time.monotonic_ns()
    deadline = clock
    pace = a.cadence * 1_000_000
    rows = 0
    events = {}
    while True:
        try:
            sample_at = time.monotonic_ns()
            count = max(0, tcp_inuse(h["sock"]) - info["listeners"])
            cap = budget(a.app, a.guest, count)
            before = sample_number(h["current"])
            started = time.monotonic_ns()
            if cap != last_cap:
                raw = (str(cap) + "\n").encode()
                require(os.pwrite(h["max"], raw, 0) == len(raw), "short memory.max write")
            write_time = time.monotonic_ns() - started
            require(
                sample_number(h["max"]) == cap
                and sample_number(h["app_max"]) == a.app
                and sample_number(h["app_swap"]) == 0,
                "parent cap readback / original service settings mismatch",
            )
            swap = sample_number(h["swap"])
            require(swap == 0, "service used swap")
            after = sample_number(h["current"])
            events = current_events(h["events"])
            stats = current_events(h["memory_stat"])
            opens = active_opens(h["snmp"])
            require(
                opens == info["initial_active_opens"],
                "service namespace made new active TCP connections, preventing passive-only calibration",
            )
            emit(
                "sample",
                n=rows,
                count=count,
                listener_baseline=info["listeners"],
                cap=cap,
                original_app=a.app,
                guest=a.guest,
                untracked_bytes=count * PER_SOCKET,
                before=before,
                current=after,
                swap=swap,
                events=events,
                app_current=sample_number(h["app_current"]),
                app_events=current_events(h["app_events"]),
                parent_events_local=current_events(h["parent_local"]),
                app_events_local=current_events(h["app_local"]),
                active_opens=opens,
                parent_psi_us=pressure_totals(h["parent_psi"]),
                app_psi_us=pressure_totals(h["app_psi"]),
                reclaim={k: stats.get(k, 0) for k in ("pgscan", "pgsteal", "anon", "file", "slab", "sock")},
                write_ns=write_time,
                changed=cap != last_cap,
                interval_ns=sample_at - last_at if last_at else 0,
                count_change=count - previous if previous is not None else 0,
                cycle_ns=time.monotonic_ns() - sample_at,
                manager_cpu_ns=time.process_time_ns(),
            )
        except OSError:
            if not select.select([h["pid"]], [], [], 0)[0]:
                raise
            finish_monitor(h, "service_exit", rows, events)
            return
        if rows == 0:
            print("READY", file=sys.stderr, flush=True)
        rows += 1
        last_cap = cap
        previous = count
        last_at = sample_at
        deadline += pace
        now = time.monotonic_ns()
        if deadline < now:
            deadline = now
        events_wait = poller.poll(max(0, (deadline - now + 999999) // 1000000))
        if events_wait:
            why = "service_exit" if any(fd == h["pid"] for fd, _ in events_wait) else "driver_closed"
            finish_monitor(h, why, rows, events)
            return


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("pid", "cpu", "app-cpu", "port"):
        parser.add_argument("--" + flag, type=int, required=True)
    parser.add_argument("--marker", required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--app", type=int, default=APP)
    parser.add_argument("--guest", type=int, default=GUEST)
    parser.add_argument("--cadence", type=int, default=CADENCE_MS)
    main(parser.parse_args())
