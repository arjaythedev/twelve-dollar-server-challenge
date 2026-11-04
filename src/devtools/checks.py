"""Tooling, native CTest and HTTP acceptance checks"""

import http.client
import os
import shlex
import shutil
import subprocess
import tarfile
import tempfile
from contextlib import closing
from pathlib import Path

from . import runtime, sources


def cpp_editor(image: str) -> None:
    """Give Windows IntelliSense the headers used by the Linux build"""
    destination = runtime.ROOT / "src/.build/editor"
    destination.mkdir(parents=True, exist_ok=True)

    version = runtime.command("docker", "run", "--rm", image, "g++", "-dumpversion")
    target = runtime.command("docker", "run", "--rm", image, "g++", "-dumpmachine")
    gcc = runtime.command("docker", "run", "--rm", image, "g++", "-print-file-name=include")
    directories = {
        "cxx": f"/usr/include/c++/{version}",
        "cxx-target": f"/usr/include/{target}/c++/{version}",
        "gcc": gcc,
        "local": "/usr/local/include",
        "system": "/usr/include",
        "uring": "/build/out/_deps/liburing/src/include",
        "llhttp": "/build/out/_deps/llhttp-src/include",
    }
    script = "set -eu\nmkdir -p /tmp/editor/sqlite /tmp/editor/yyjson\n"
    for name, source in directories.items():
        script += f"cp -aL {shlex.quote(source)} /tmp/editor/{name}\n"
    script += (
        "cp /build/out/_deps/sqlite-src/sqlite3.h /build/out/_deps/sqlite-src/sqlite3ext.h "
        "/tmp/editor/sqlite/\n"
        "cp /build/out/_deps/yyjson_source-src/src/yyjson.h /tmp/editor/yyjson/\n"
        "tar -cf - -C /tmp/editor .\n"
    )
    with tempfile.TemporaryFile() as archive:
        subprocess.run(
            ["docker", "run", "--rm", "--network", "none", image, "sh", "-c", script],
            stdout=archive,
            check=True,
        )
        archive.seek(0)
        with tarfile.open(fileobj=archive) as headers:
            headers.extractall(destination, filter="data")

    print("Editor headers refreshed from the Linux build image.")


def native_check(image: str) -> None:
    if os.name == "nt":
        cpp_editor(image)
    print(
        runtime.command(
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--read-only",
            "--tmpfs=/tmp:rw,mode=1777,size=64m",
            "--tmpfs=/build/out/Testing:rw,mode=1777,size=32m",
            "--user",
            "10001:10001",
            "--cap-drop",
            "ALL",
            "--cpus",
            "1",
            "--memory",
            "2g",
            "--memory-swap",
            "2g",
            "--ulimit",
            "nofile=65535:65535",
            "--security-opt",
            f"seccomp={runtime.ROOT / 'src/docker/seccomp.json'}",
            "--entrypoint",
            "ctest",
            image,
            "--test-dir",
            "/build/out",
            "--output-on-failure",
            timeout=None,
        )
    )


def check(use_docker: bool) -> None:
    for args in [
        ["ruff", "check", "."],
        ["ruff", "format", "--check", "."],
        ["pyright"],
        ["python", "-m", "unittest", "discover", "-v", "-s", "tests", "-t", ".", "-p", "test_*.py"],
    ]:
        subprocess.run(["uv", "run", "--locked", *args], cwd=runtime.ROOT / "src", check=True)
    if use_docker:
        with sources.capture(runtime.ROOT) as source:
            test_image = runtime.build("test-build", source=source)
            native_check(test_image)
            image = runtime.build("tested-runtime", source=source)
            tested = runtime.image_identity(test_image, "/build/out/server")
            serving = runtime.image_identity(image, "/app/server")
            if tested["binary_sha256"] != serving["binary_sha256"]:
                raise RuntimeError("HTTP checks must use the exact server tested by CTest")
            output = runtime.ROOT / "src/.build/checks"
            output.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="check-", dir=output) as directory:
                work = Path(directory).resolve()
                with runtime.server(work, image=image, workspace=source.root) as (container, port):
                    verify(container, port, work)
        subprocess.run(
            ["uv", "run", "--locked", "python", "-m", "unittest", "-v", "tests.test_api"],
            cwd=runtime.ROOT / "src",
            env=os.environ
            | {"TDS_HTTP_TESTS": "1", "TDS_HTTP_TEST_IMAGE": str(serving["image_id"])},
            check=True,
        )


def verify(container: str, port: int, output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="official-", dir=output) as directory:
        work = Path(directory).resolve()
        if not work.is_relative_to(output.resolve()):
            raise RuntimeError("official check directory must stay under its output directory")
        # Only normalize checkout CRLF. Preserve every assertion and golden byte
        (work / "test.sh").write_text(
            (runtime.ROOT / "test/test.sh").read_text(encoding="utf-8"),
            encoding="utf-8",
            newline="\n",
        )
        shutil.copytree(runtime.ROOT / "test/golden", work / "golden")
        runtime.command("docker", "cp", str(work), f"{container}:/tmp/official")
        result = runtime.command("docker", "exec", container, "bash", "/tmp/official/test.sh")
        (output / "official-tests.txt").write_text(result + "\n", encoding="utf-8")
        if "passed=42 failed=0" not in result:
            raise RuntimeError(result)
    with closing(http.client.HTTPConnection("127.0.0.1", port, timeout=5)) as connection:
        connection.request("GET", "/health")
        first = connection.getresponse()
        first.read()
        first_socket = connection.sock
        connection.request("GET", "/feed")
        second = connection.getresponse()
        second.read()
        if (
            first.status != 200
            or second.status != 200
            or first_socket is None
            or connection.sock is not first_socket
        ):
            raise RuntimeError("keep-alive check failed")
    print("42 official checks passed; keep-alive connection reused.")
