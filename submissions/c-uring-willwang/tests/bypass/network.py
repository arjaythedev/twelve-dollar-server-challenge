"""One link between two private Docker network namespaces, with no host-side veth."""

import array
from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import sys
import tempfile

HERE = Path(__file__).resolve().parent
VETH = HERE.parent / "official/veth"
sys.path.insert(0, str(VETH))
from common import docker, group_of, subnet_request, wait_path
from probe import PrivatePairs

BASE_IP = "10.241.1.1"
STACK_IP = "10.241.1.253"
CLIENT_IP = "10.241.1.254"


class PrivateLink(PrivatePairs):
    def base(self, name):
        options = super().base(name)
        if name == "server0":
            # Linux must drop IP packets which only the userspace stack owns.
            options += ["--sysctl", "net.ipv4.ip_forward=0"]
        return options

    def build(self):
        super().build()
        self.client_root = self.root / "client-sockets"
        self.client_root.mkdir()
        (self.client_root / "ep0").symlink_to(self.root / "client0", target_is_directory=True)

    def client_env(self):
        return dict(
            os.environ,
            LD_PRELOAD=str(self.work / "veth-bin/allocate.so"),
            TWELVE_TEST_SOCKET_ROOT=str(self.client_root),
            TWELVE_TEST_SOCKET_COUNT="1",
            NO_PROXY="*",
            no_proxy="*",
        )

    def server_counters(self):
        return {
            key: int(value)
            for key, value in (
                field.split("=") for field in subnet_request(self.providers[0], b"T").split()
            )
        }

    @contextmanager
    def broker(self, mode):
        if mode not in ("baseline", "packet"):
            raise ValueError("unknown descriptor backend")
        with tempfile.TemporaryDirectory(prefix="fd-", dir=self.root) as directory:
            control = Path(directory)
            control.chmod(0o777)
            name = control.name
            options = ["--cap-add", "NET_RAW"] if mode == "packet" else []
            owner = docker(
                "run",
                "-d",
                *self.base(name),
                "--network",
                "container:" + self.server_ids[0],
                "--cpuset-cpus",
                str(self.server_cpu),
                "--memory",
                "12m",
                "--memory-swap",
                "12m",
                "--pids-limit",
                "8",
                *options,
                "--mount",
                f"type=bind,src={self.work/'packet-fd'},dst=/packet-fd,readonly",
                "--mount",
                f"type=bind,src={control},dst=/control",
                "--entrypoint",
                "/packet-fd",
                self.image,
                mode,
                str(self.host_inode),
            )
            try:
                wait_path(control / "packet.sock", lambda: docker("logs", owner))
                pid = int(docker("inspect", "--format", "{{.State.Pid}}", owner))
                yield control / "packet.sock", group_of(pid)
            finally:
                docker("rm", "-f", owner, check=False)


def receive(path):
    # Keep this local control connection open until the server exits. The
    # capability-free broker then remains in its original measurable cgroup.
    control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        control.settimeout(5)
        control.connect(str(path))
        payload, ancillary, flags, _ = control.recvmsg(512, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        fds = array.array("i")
        for level, kind, value in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds.frombytes(value)
        if len(fds) != 1 or flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC):
            for fd in fds:
                os.close(fd)
            raise RuntimeError("expected one complete private descriptor")
        try:
            meta = json.loads(payload)
            if meta["driver"] != "veth" or meta["mtu"] != 1500 or meta["effective_caps"]:
                raise ValueError(f"unexpected private link metadata: {meta}")
        except BaseException:
            os.close(fds[0])
            raise
        return control, fds[0], meta
    except BaseException:
        control.close()
        raise
