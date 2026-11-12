"""Create one private veth between two cooperating, disposable Docker endpoints."""

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from common import collect_dir, docker, tcp_socket, wait_path, stop, validate_cpus


def cpumask(cpus):
    val = sum(1 << c for c in cpus)
    return ",".join(f"{(val>>x)&0xffffffff:08x}" for x in reversed(range(0, val.bit_length(), 32)))


class PrivatePairs:
    def __init__(
        self, root, output, server_cpu, groups, aliases, work, tokens, manager_cpu, targets_per_client=1
    ):
        self.root = root
        self.output = output
        self.work = work
        self.tokens = tokens
        self.manager_cpu = manager_cpu
        self.server_cpu = server_cpu
        self.groups = groups
        self.aliases = aliases
        self.targets_per_client = targets_per_client
        self.unique = "twelve-veth-" + uuid.uuid4().hex[:9]
        self.label = "org.twelve-dollar.veth=" + self.unique
        self.containers = []
        self.providers = []
        self.server_ids = []
        self.client_ids = []
        self.client_providers = []
        self.reports = []
        self.image = None
        self.host_inode = os.stat("/proc/self/ns/net").st_ino
        self.initial_host_mask = Path("/proc/sys/net/core/rps_default_mask").read_text().strip()

    def __enter__(self):
        try:
            self.build()
            return self
        except BaseException:
            self.close()
            raise

    def __exit__(self, *exc):
        self.close()

    def build(self):
        path = self.root / "image.tar"
        tools = self.work / "veth-bin"
        sources = {name: tools / name for name in ("setup", "show", "probe", "endpoint", "hold")}
        sources.update(mload=self.work / "mload")
        sources["tokens.json"] = self.tokens
        with tarfile.open(path, "w") as ar:
            for name, src in sources.items():
                info = tarfile.TarInfo(name)
                info.size = src.stat().st_size
                info.mode = 0o555
                with src.open("rb") as fp:
                    ar.addfile(info, fp)
        self.image = docker("image", "import", "--change", "LABEL " + self.label, str(path))
        for k in range(len(self.groups) * self.targets_per_client):
            self.pair(k, self.groups[k // self.targets_per_client])
        current = Path("/proc/sys/net/core/rps_default_mask").read_text().strip()
        if self.initial_host_mask != current:
            raise RuntimeError("host steering default changed")

    def base(self, name):
        return [
            "--pull",
            "never",
            "--read-only",
            "--security-opt",
            "no-new-privileges",
            "--cap-drop",
            "ALL",
            "--label",
            self.label,
            "--name",
            self.unique + "-" + name,
        ]

    def pair(self, k, group):
        pair = collect_dir(self.root, "pair" + str(k))
        epdir = collect_dir(self.root, "ep" + str(k))
        group_index, part = divmod(k, self.targets_per_client)
        common = ["--network", "none", "--user", "65532:65532", "--pids-limit", "96"]
        server = docker(
            "run",
            "-d",
            *self.base("server" + str(k)),
            *common,
            "--memory",
            "8m",
            "--memory-swap",
            "8m",
            "--sysctl",
            "net.core.rps_default_mask=" + cpumask([self.server_cpu]),
            "--cpuset-cpus",
            str(self.server_cpu),
            "--mount",
            f"type=bind,src={epdir},dst=/control",
            "--entrypoint",
            "/endpoint",
            self.image,
        )
        self.containers.append(server)
        self.server_ids.append(server)
        provider = epdir / "new.sock"
        wait_path(provider, lambda: docker("logs", server))
        self.providers.append(provider)
        if part == 0:
            gd = collect_dir(self.output, "go" + str(group_index))
            cp = collect_dir(self.root, "client" + str(group_index))
            client = docker(
                "run",
                "-d",
                *self.base("client" + str(group_index)),
                *common,
                "--sysctl",
                "net.core.rps_default_mask=" + cpumask(group),
                "--cpuset-cpus",
                ",".join(map(str, group)),
                "--ulimit",
                "nofile=262144:262144",
                "--memory",
                "8g",
                "--memory-swap",
                "8g",
                "--mount",
                f"type=bind,src={gd},dst=/out",
                "--mount",
                f"type=bind,src={cp},dst=/control",
                "--entrypoint",
                "/endpoint",
                self.image,
            )
            self.containers.append(client)
            self.client_ids.append(client)
            self.reports.append(gd)
            self.client_providers.append(cp / "new.sock")
            wait_path(cp / "new.sock", lambda: docker("logs", client))
        else:
            client = self.client_ids[group_index]

        def setup(role, anchor, extra):
            return [
                "run",
                *extra,
                *self.base("setup" + role + str(k)),
                "--network",
                "container:" + anchor,
                "--cap-add",
                "NET_ADMIN",
                "--cpuset-cpus",
                str(self.manager_cpu),
                "--pids-limit",
                "8",
                "--memory",
                "12m",
                "--memory-swap",
                "12m",
                "--mount",
                f"type=bind,src={pair},dst=/pair",
                "--entrypoint",
                "/setup",
                self.image,
                role,
                str(k + 1),
                str(self.aliases),
                str(self.host_inode),
                str(part),
            ]

        starter = docker(*setup("server", server, ["-d"]))
        self.containers.append(starter)
        wait_path(pair / "setup.sock", lambda: docker("logs", starter))
        clientlog = docker(*setup("client", client, ["--rm"]))
        if docker("wait", starter) != "0":
            raise ValueError("server private configuration: " + docker("logs", starter))
        initlog = docker("logs", starter)
        if not all("final effective=0 permitted=0" in log for log in [clientlog, initlog]):
            raise ValueError("late configuration privileges")
        docker("rm", starter)
        self.containers.remove(starter)
        for role, container, cpus in (("server", server, [self.server_cpu]), ("client", client, group)):
            s = docker("exec", container, "/show")
            device = "bench0" if role == "server" else "bench" + str(part)
            matches = [z.split()[-1] for z in s.splitlines() if f"/{device}/queues/rx-0/rps_cpus " in z]
            if len(matches) != 1 or int(matches[0].replace(",", ""), 16) != sum(1 << c for c in cpus):
                raise ValueError("wrong active private veth receiver mask: " + s)
            print(f"pair{k} {role} private {device} rps={matches[0]}", flush=True)

    def close(self):
        for c in reversed(self.containers):
            docker("rm", "-f", c, check=False)
        if self.image:
            docker("image", "rm", self.image, check=False)


def main(args):
    validate_cpus(args.manager_cpu, args.server_cpu, [[args.client_cpu]])
    if args.out.exists():
        raise ValueError("use a new output directory")
    args.out.mkdir(mode=0o700, parents=True)
    with tempfile.TemporaryDirectory(prefix="twelve-veth-") as tmp:
        with PrivatePairs(
            Path(tmp),
            args.out,
            args.server_cpu,
            [[args.client_cpu]],
            args.aliases,
            args.work.resolve(),
            args.tokens.resolve(),
            args.manager_cpu,
        ) as pairs:
            if args.mode == "info":
                return
            fd = tcp_socket(pairs.providers[0])
            server = None
            try:
                env = dict(os.environ, TWELVE_TEST_LISTENER_FD=str(fd))
                server = subprocess.Popen(
                    [
                        "taskset",
                        "-c",
                        str(args.server_cpu),
                        str(args.work.resolve() / "veth-bin/probe"),
                        "server",
                        "10.241.1.1",
                        "19447",
                    ],
                    env=env,
                    pass_fds=(fd,),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                start = server.stdout.readline()
                if not start.startswith("listening "):
                    raise ValueError("native listener failed: " + start)
                ct = Path("/proc/sys/net/netfilter/nf_conntrack_count")
                before = ct.read_text().strip()
                client = docker(
                    "exec", pairs.client_ids[0], "/probe", "client", "10.241.1.1", "19447", timeout=8
                )
                native = server.communicate(timeout=3)[0]
                after = ct.read_text().strip()
                (args.out / "server.log").write_text(start + native)
                (args.out / "client.log").write_text(client + "\n")
                if (
                    f"incoming_cpu={args.server_cpu}:24," not in native
                    or f"incoming_cpu={args.client_cpu}:24," not in client
                ):
                    raise ValueError("native veth receive placement: " + native + " " + client)
                print(native + client, flush=True)
                print("host conntrack before/after", before, after, flush=True)
            finally:
                stop(server)
                os.close(fd)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("info", "placement"))
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--tokens", type=Path, required=True)
    ap.add_argument("--manager-cpu", type=int, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--server-cpu", type=int, required=True)
    ap.add_argument("--client-cpu", type=int, required=True)
    ap.add_argument("--aliases", type=int, default=12)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    main(ap.parse_args())
