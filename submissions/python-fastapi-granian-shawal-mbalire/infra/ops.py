#!/usr/bin/env python3
"""The challenge's build / start steps, in Python.

build.sh and start.sh are required by the challenge and run with bash, so each is a one-line shim
into this file (install.sh is a single apt-get line, since it has to install python3 first).
Stdlib only, and Python 3.9 compatible: it runs before the venv exists.

    python3 infra/ops.py build     # normal user: .venv with the pinned requirements (see build())
    python3 infra/ops.py start     # foreground: exec Granian. Env: SQLITE_PATH, JWT_SECRET, HOST, PORT
"""

import hashlib
import os
import platform
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The server's interpreter: CPython from python-build-standalone (clang, PGO + LTO, and SQLite 3.53
# instead of Ubuntu's 3.45). Its 3.15 ran the request mix ~15% faster in-process than Ubuntu's 3.12
# (13.0-13.5 vs 15.4-16.1 µs per request). Pinned by release and SHA-256.
# 3.11, not the latest (3.15): Granian's Rust task implementation (--task-impl rust) only runs on
# 3.11, and the pair cost 3-7% less per request on the bench (2026-10-16; AGENTS.md). Owner's choice.
PYTHON_RELEASE = "20261013"
PYTHON_VERSION = "3.11.17"
PYTHON_SHA256 = {
    "aarch64": "643954c07b04c18e215f812691cd5fa15d924fec1f3a5fdd9c1789484382ac24",
    "x86_64": "846bf682e2b67393f2affa5e5c0994e446abc3a7cc5bb699890c36346823644c",
}
PYTHON_URL = ("https://github.com/astral-sh/python-build-standalone/releases/download/{release}/"
              "cpython-{version}+{release}-{arch}-unknown-linux-gnu-install_only.tar.gz")
PYTHON_HOME = ROOT / ".python"

# One worker on a single-threaded runtime: the box has 1 vCPU and SQLite has one connection.
GRANIAN_FLAGS = [
    "--interface", "asgi",  # asgi, not asginl: lifespan opens/closes the store and the profiler
    "--workers", "1",
    "--runtime-mode", "st",
    "--loop", "uvloop",
    "--task-impl", "rust",  # Granian's "experimental" Rust tasks; on Python >= 3.12 it falls back to asyncio
    "--http", "1",
    "--no-ws",
    "--no-access-log",
    "--backlog", "4096",
    # Granian holds one backpressure permit per open connection (default backlog/workers = 4096),
    # so with every user on its own keep-alive socket, connection 4097 onward waited unserved.
    # The descriptor limit (65535) and adapters/descriptor_budget.py are the real cap.
    "--backpressure", "65535",
    "--http1-header-read-timeout", "60000",  # ms; Granian's maximum
]


def run(*cmd: str, **env: str) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, env={**os.environ, **env})


def standalone_python() -> str:
    """Downloads and unpacks the pinned interpreter into .python/ (once); returns its python3."""
    python = PYTHON_HOME / "python" / "bin" / "python3"
    marker = PYTHON_HOME / (PYTHON_VERSION + "+" + PYTHON_RELEASE)
    if marker.exists() and python.exists():
        return str(python)
    arch = platform.machine()
    if arch not in PYTHON_SHA256:
        raise SystemExit("no pinned python-build-standalone build for " + arch)
    url = PYTHON_URL.format(release=PYTHON_RELEASE, version=PYTHON_VERSION, arch=arch)
    print("+ download", url, flush=True)
    archive = urllib.request.urlopen(url, timeout=120).read()
    if hashlib.sha256(archive).hexdigest() != PYTHON_SHA256[arch]:
        raise SystemExit("the downloaded interpreter does not match its pinned SHA-256")
    subprocess.run(["rm", "-rf", str(PYTHON_HOME)], check=True)
    PYTHON_HOME.mkdir()
    archive_path = PYTHON_HOME / "python.tar.gz"
    archive_path.write_bytes(archive)
    with tarfile.open(archive_path) as tar:
        if hasattr(tarfile, "tar_filter"):  # 3.12+: refuse paths outside .python/
            tar.extractall(PYTHON_HOME, filter="tar")
        else:
            tar.extractall(PYTHON_HOME)
    archive_path.unlink()
    marker.touch()
    return str(python)


def build() -> None:
    """.venv from the pinned interpreter on Linux; PYTHON (or this interpreter) elsewhere, e.g. a Mac."""
    if os.environ.get("PYTHON"):
        python = os.environ["PYTHON"]
    elif sys.platform == "linux":
        python = standalone_python()
    else:
        python = sys.executable
    run(python, "-m", "venv", "--clear", str(ROOT / ".venv"))
    run(str(ROOT / ".venv/bin/pip"), "install", "--disable-pip-version-check", "--no-input", "--quiet",
        "--only-binary=:all:", "-r", str(ROOT / "requirements.txt"))


def start() -> None:
    missing = [name for name in ("SQLITE_PATH", "JWT_SECRET", "HOST", "PORT") if not os.environ.get(name)]
    if missing:
        raise SystemExit("missing environment variables: " + ", ".join(missing))
    os.chdir(ROOT)
    granian = str(ROOT / ".venv/bin/granian")
    os.execv(granian, [granian, *GRANIAN_FLAGS, "--host", os.environ["HOST"], "--port", os.environ["PORT"], "main:app"])


COMMANDS = {"build": build, "start": start}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        raise SystemExit("usage: python3 infra/ops.py " + "|".join(COMMANDS))
    COMMANDS[sys.argv[1]]()
