#!/usr/bin/env python3
"""The challenge's build / start steps, in Python.

build.sh and start.sh are required by the challenge and run with bash, so each is a one-line shim
into this file (install.sh is a single apt-get line, since it has to install python3 first).
Stdlib only, and Python 3.9 compatible: Ubuntu's python3 (or a Mac's) runs it, not the server's interpreter.

    python3 infra/ops.py build     # normal user: the pinned interpreter in .python/ (Linux; see build())
    python3 infra/ops.py start     # foreground: exec main.py. Env: SQLITE_PATH, JWT_SECRET, HOST, PORT

The server needs no third-party package, so there is no venv and no requirements.txt.
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

# The server's interpreter: CPython 3.15 from python-build-standalone (clang, PGO + LTO, the tail-call
# interpreter, SQLite 3.53 instead of Ubuntu's 3.45), pinned by release and SHA-256.
PYTHON_RELEASE = "20261013"
PYTHON_VERSION = "3.15.0"
PYTHON_SHA256 = {
    "aarch64": "26a54e8fbde81bcc39ba35ad855a6a573295c023793c20fff1670a560649bcd7",
    "x86_64": "e02a368dd5dc76ffe0599e5109ad6c1ee84e597910758b0e006bab25372a8921",
}
PYTHON_URL = ("https://github.com/astral-sh/python-build-standalone/releases/download/{release}/"
              "cpython-{version}+{release}-{arch}-unknown-linux-gnu-install_only.tar.gz")
PYTHON_HOME = ROOT / ".python"
STANDALONE = PYTHON_HOME / "python" / "bin" / "python3"
MINIMUM = (3, 14)  # PEP 695 syntax and deferred annotations (PEP 649)


def standalone_python() -> str:
    """Downloads and unpacks the pinned interpreter into .python/ (once); returns its python3."""
    marker = PYTHON_HOME / (PYTHON_VERSION + "+" + PYTHON_RELEASE)
    if marker.exists() and STANDALONE.exists():
        return str(STANDALONE)
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
    return str(STANDALONE)


def server_python() -> str:
    """The pinned interpreter once built (Linux); else $PYTHON, else this interpreter (e.g. on a Mac)."""
    if STANDALONE.exists() and not os.environ.get("PYTHON"):
        return str(STANDALONE)
    return os.environ.get("PYTHON") or sys.executable


def check_version(python: str) -> None:
    probe = "import sys; sys.exit(sys.version_info < %r)" % (MINIMUM,)
    if subprocess.run([python, "-c", probe]).returncode != 0:
        raise SystemExit("%s is older than Python %d.%d; set PYTHON" % ((python,) + MINIMUM))


def build() -> None:
    """The pinned interpreter on Linux; elsewhere PYTHON (or this interpreter) only has to be new enough."""
    python = standalone_python() if sys.platform == "linux" and not os.environ.get("PYTHON") else server_python()
    check_version(python)
    print("+ server interpreter:", python, flush=True)


def start() -> None:
    missing = [name for name in ("SQLITE_PATH", "JWT_SECRET", "HOST", "PORT") if not os.environ.get(name)]
    if missing:
        raise SystemExit("missing environment variables: " + ", ".join(missing))
    os.chdir(ROOT)
    python = server_python()
    os.execv(python, [python, str(ROOT / "main.py")])


COMMANDS = {"build": build, "start": start}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        raise SystemExit("usage: python3 infra/ops.py " + "|".join(COMMANDS))
    COMMANDS[sys.argv[1]]()
