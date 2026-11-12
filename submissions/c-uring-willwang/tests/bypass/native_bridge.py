"""Place the native veth XDP poll on the server CPU through a private, kernel-only L2 hop."""

import http.client
import json
import os
from pathlib import Path
import re
import shutil
import socket

from common import collect_dir, docker, group_of, subnet_request, tcp_socket, wait_path
from network import BASE_IP, CLIENT_IP
from probe import cpumask
from xdp_service import mount, runtime_libraries


def prepare(link):
    """Interpose the private bridge before the application or client source routes are configured."""
    if hasattr(link, "bridge_group"):
        return link.bridge_group
    if len(link.server_ids) != 1 or len(link.client_ids) != 1 or hasattr(link, "source_routes"):
        raise ValueError("prepare the one-link native bridge before the application or source routes")
    if not Path("/sys/module/bridge").is_dir():
        raise RuntimeError("the Linux bridge module must already be present")
    ip = shutil.which("ip")
    if not ip:
        raise RuntimeError("the private bridge setup requires the installed iproute2 utility")

    # The original server endpoint becomes the L2 hop. Its receive queues were
    # created inside a namespace with RPS set to exactly the server CPU.
    middle = link.server_ids[0]
    old_control = link.root / "ep0"
    middle_control = link.root / "bridge-control"
    old_control.rename(middle_control)
    new_control = collect_dir(link.root, "ep0")
    pair = collect_dir(link.root, "bridge-pair")
    # Keep deterministic cleanup handles even if Docker creates a container but
    # a later part of its start fails.
    link.containers.append(link.unique + "-xdp-receiver")
    server = docker("run", "-d", *link.base("xdp-receiver"), "--network", "none",
                    "--sysctl", "net.ipv4.ip_forward=0",
                    "--sysctl", "net.core.rps_default_mask=" + cpumask([link.server_cpu]),
                    "--cpuset-cpus", str(link.server_cpu), "--memory", "8m", "--memory-swap", "8m",
                    "--user", "65532:65532", "--pids-limit", "8",
                    "--mount", f"type=bind,src={new_control},dst=/control",
                    "--entrypoint", "/endpoint", link.image)
    wait_path(new_control / "new.sock", lambda: docker("logs", server))

    def pair_setup(role, owner, index, detach):
        return docker("run", "-d" if detach else "--rm", *link.base("bridge-" + role),
                      "--network", "container:" + owner, "--cap-add", "NET_ADMIN",
                      "--cpuset-cpus", str(link.manager_cpu), "--memory", "12m", "--memory-swap", "12m",
                      "--pids-limit", "8", "--mount", f"type=bind,src={pair},dst=/pair",
                      "--entrypoint", "/setup", link.image, role, "1", "1", str(link.host_inode), str(index))

    setup_name = link.unique + "-bridge-server"
    link.containers.append(setup_name)
    setup = pair_setup("server", server, 0, True)
    wait_path(pair / "setup.sock", lambda: docker("logs", setup))
    client_log = pair_setup("client", middle, 1, False)
    status = docker("wait", setup)
    server_log = docker("logs", setup)
    if status != "0" or any("final effective=0 permitted=0" not in log for log in (client_log, server_log)):
        raise RuntimeError("the second private veth was not configured correctly")
    inodes = [int(re.search(r"configured own namespace (\d+),", log)[1]) for log in (client_log, server_log)]
    if len({link.host_inode, *inodes}) != 3:
        raise RuntimeError("expected independent middle and receiving namespaces")
    docker("rm", setup)
    link.containers.remove(setup_name)

    # iproute2 runs with only NET_ADMIN inside the middle endpoint. All
    # commands explicitly name its two just-created private veth ports.
    mounts = mount(ip, "/private-ip")
    for lib in runtime_libraries(ip):
        mounts += mount(lib, str(lib))
    operations = pair / "bridge.ip"
    operations.write_text(
        f"address del {BASE_IP}/24 dev bench0\n"
        f"address del {CLIENT_IP}/24 dev bench1\n"
        "link add name transit0 type bridge stp_state 0 mcast_snooping 0\n"
        "link set dev bench0 master transit0\n"
        "link set dev bench1 master transit0\n"
        "link set dev transit0 up\n"
    )
    base = ["run", "--rm", *link.base("bridge-ip"), "--network", "container:" + middle,
            "--cpuset-cpus", str(link.manager_cpu), "--memory", "12m", "--memory-swap", "12m",
            "--pids-limit", "8", *mounts]
    docker(*base, "--cap-add", "NET_ADMIN", *mount(operations, "/private-bridge.ip"),
           "--entrypoint", "/private-ip", link.image, "-4", "-batch", "/private-bridge.ip")
    ports = json.loads(docker(*base, "--entrypoint", "/private-ip", link.image,
                              "-j", "-details", "link", "show", "master", "transit0"))
    ipv4 = json.loads(docker(*base, "--entrypoint", "/private-ip", link.image, "-j", "-4", "addr"))
    if ({p["ifname"] for p in ports} != {"bench0", "bench1"} or
            any(p["linkinfo"]["info_kind"] != "veth" or
                p["linkinfo"]["info_slave_data"]["state"] != "forwarding" for p in ports) or
            any(a.get("addr_info") for a in ipv4 if a["ifname"] != "lo")):
        raise RuntimeError("the private bridge must have exactly two forwarding ports and no IPv4 address")
    masks = {}
    for role, owner, ports_to_check, cpus in (
            ("middle", middle, ("bench0", "bench1"), [link.server_cpu]),
            ("server", server, ("bench0",), [link.server_cpu]),
            ("client", link.client_ids[0], ("bench0",), link.groups[0])):
        masks[role] = docker("exec", owner, "/show")
        for name in ports_to_check:
            values = [int(line.split()[-1].replace(",", ""), 16) for line in masks[role].splitlines()
                      if f"/{name}/queues/rx-0/rps_cpus " in line]
            if values != [sum(1 << cpu for cpu in cpus)]:
                raise RuntimeError(f"private {role} port {name} has unexpected RPS: {masks[role]}")
    link.server_ids[0] = server
    link.bridge_provider = middle_control / "new.sock"
    pid = int(docker("inspect", "--format", "{{.State.Pid}}", middle))
    link.bridge_group = group_of(pid)
    memory = int((link.bridge_group / "memory.max").read_text())
    swap = int((link.bridge_group / "memory.swap.max").read_text())
    if memory != 8 * 2**20 or swap or os.sched_getaffinity(pid) != {link.server_cpu}:
        raise RuntimeError("the private bridge helper has an unexpected CPU, memory or swap budget")
    data = {"middle_container": middle, "server_container": server, "client_container": link.client_ids[0],
            "middle_netns_inode": inodes[0], "server_netns_inode": inodes[1], "cpu": link.server_cpu,
            "ports": ports, "middle_ipv4": ipv4, "queue_masks": masks,
            "middle_cgroup": str(link.bridge_group), "middle_memory_max": memory, "middle_swap_max": swap,
            "initial_middle_conntrack": subnet_request(link.bridge_provider, b"?")}
    (link.output / "private-bridge.json").write_text(json.dumps(data, indent=2) + "\n")
    return link.bridge_group


def verify_replies(link, target, port):
    """Check the receive CPU of four real HTTP health replies before the stock scorer starts."""
    affinity = os.sched_getaffinity(0)
    cpus = link.groups[0]
    received = []
    try:
        os.sched_setaffinity(0, {cpus[0]})
        for _ in range(4):
            with socket.socket(fileno=tcp_socket(link.client_providers[0])) as client:
                client.settimeout(3)
                client.connect((target, port))
                client.sendall(b"GET /health HTTP/1.1\r\nHost: private\r\nConnection: close\r\n\r\n")
                with http.client.HTTPResponse(client) as reply:
                    reply.begin()
                    if reply.status != 200 or json.loads(reply.read()).get("db") != "ok":
                        raise RuntimeError("private bridge HTTP placement preflight failed")
                received.append(client.getsockopt(socket.SOL_SOCKET, socket.SO_INCOMING_CPU))
    finally:
        os.sched_setaffinity(0, affinity)
    result = {"request": "GET /health", "before_stock_scorer": True, "sender_cpu": cpus[0],
              "client_cpus": cpus, "reply_incoming_cpus": received}
    (link.output / "bridge-reply-placement.json").write_text(json.dumps(result, indent=2) + "\n")
    if not all(cpu in cpus for cpu in received):
        raise RuntimeError(f"private bridge replies ran outside the client CPUs: {received}")
    return result
