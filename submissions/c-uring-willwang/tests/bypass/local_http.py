"""Run the stock goldens on localhost and check writes across both service transports."""

import http.client
import json
import shutil
import socket

from common import docker, tcp_socket
from lab import PORT, REPO
from xdp_service import mount, runtime_libraries


def stock_localhost(link, out):
    commands = ("bash", "curl", "jq", "openssl", "basename", "dirname", "mktemp", "rm", "cat",
                "grep", "head", "cut", "tr", "date", "seq")
    binaries = {name: shutil.which(name) for name in commands}
    if not all(binaries.values()):
        raise ValueError("the stock HTTP test requires " + ", ".join(name for name, path in binaries.items() if not path))
    mounts = mount(REPO / "test", "/test")
    for name, path in binaries.items():
        mounts += mount(path, "/usr/bin/" + name)
    for path in runtime_libraries(*binaries.values()):
        mounts += mount(path, str(path))
    try:
        output = docker("run", "--rm", *link.base("localhost-golden"),
                        "--network", "container:" + link.server_ids[0], "--user", "65532:65532",
                        "--cpuset-cpus", str(link.groups[0][0]), "--pids-limit", "48",
                        "--memory", "64m", "--memory-swap", "64m", "--tmpfs", "/tmp:rw,nosuid,nodev,noexec,size=8m",
                        "--env", "PATH=/usr/bin", "--env", "LC_ALL=C", *mounts,
                        "--entrypoint", "/usr/bin/bash", link.image, "/test/test.sh",
                        f"http://127.0.0.1:{PORT}", timeout=60)
    except RuntimeError as exc:
        (out / "localhost-golden.log").write_text(str(exc) + "\n")
        raise
    (out / "localhost-golden.log").write_text(output + "\n")
    if output.splitlines()[-1] != "passed=42 failed=0":
        raise RuntimeError("localhost did not pass the organizer test")


def connection(provider, host):
    s = socket.socket(fileno=tcp_socket(provider))
    try:
        s.settimeout(4)
        s.connect((host, PORT))
        return s
    except BaseException:
        s.close()
        raise


def check(args, link, target, out):
    remote, local = (link.client_providers[0], target), (link.providers[0], "127.0.0.1")
    token = json.loads(args.tokens.read_text())[0]["token"]
    auth = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}

    def api(endpoint, method, path, expected, payload=None):
        provider, host = endpoint
        c = http.client.HTTPConnection(host, PORT, timeout=4)
        try:
            c.sock = connection(provider, host)
            c.request(method, path, body=json.dumps(payload).encode() if payload else None, headers=auth)
            reply = c.getresponse()
            body = json.loads(reply.read())
            if reply.status != expected:
                raise RuntimeError(f"{method} {path}: {reply.status} {body}")
            return body
        finally:
            c.close()

    before = link.server_counters()
    api(remote, "GET", "/feed", 200)
    after_remote = link.server_counters()
    stock_localhost(link, out)
    after_local = link.server_counters()
    remote_delta = {k: after_remote[k] - before[k] for k in ("InSegs", "OutSegs")}
    local_delta = {k: after_local[k] - after_remote[k] for k in ("InSegs", "OutSegs")}
    if any(remote_delta.values()) or not all(local_delta.values()):
        raise RuntimeError(f"unexpected server Linux TCP traffic: remote={remote_delta}, localhost={local_delta}")

    # The golden suite created and liked this post solely through loopback.
    newest = api(remote, "GET", "/feed", 200)["posts"][0]
    if newest["body"] != "b" * 500 or newest["like_count"] != 1:
        raise RuntimeError("remote XDP did not observe the local golden write")
    post = api(remote, "POST", "/posts", 201, {"body": "remote write visible on localhost"})["post"]
    path = f"/posts/{post['id']}"
    if api(local, "GET", path, 200)["post"] != post:
        raise RuntimeError("localhost did not observe the committed remote write")
    api(local, "POST", path + "/like", 201)
    if api(remote, "GET", path, 200)["post"]["like_count"] != 1:
        raise RuntimeError("remote XDP did not observe the local like")

    body = b'{"body":"chunked localhost write"}'
    with connection(*local) as client:
        client.sendall((f"POST /posts HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\n"
                        "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n").encode())
        for part in (body[:2], body[2:9], body[9:]):
            client.sendall(f"{len(part):x}\r\n".encode() + part + b"\r\n")
        client.sendall(b"0\r\nX-Client: local\r\n\r\n")
        response = http.client.HTTPResponse(client)
        response.begin()
        chunked = json.loads(response.read())["post"]
        if response.status != 201 or api(remote, "GET", f"/posts/{chunked['id']}", 200)["post"] != chunked:
            raise RuntimeError("remote XDP did not observe the chunked localhost write")

    # A half-close on the write side still permits both pipelined replies.
    with connection(*local) as client:
        client.sendall(b"GET /health HTTP/1.1\r\nHost: local\r\n")
        client.sendall(b"\r\nGET /health HTTP/1.1\r\nHost: local\r\n\r\n")
        client.shutdown(socket.SHUT_WR)
        received = bytearray()
        while part := client.recv(4096):
            received.extend(part)
        if received.count(b"HTTP/1.1 200 OK\r\n") != 2 or received.count(b'"db":"ok"') != 2:
            raise RuntimeError("local fragment, pipeline or half-close lost a reply")
    return {"organizer": "passed=42 failed=0", "remote_only_kernel_tcp": remote_delta,
            "localhost_only_kernel_tcp": local_delta, "remote_read_local_post": newest["id"],
            "local_read_remote_post": post["id"], "remote_read_local_like": 1,
            "chunked_local_post": chunked["id"], "half_close_responses": 2}
