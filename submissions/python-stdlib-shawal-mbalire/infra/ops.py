#!/usr/bin/env python3
"""The challenge's build / start steps, in Python.

build.sh and start.sh are required by the challenge and run with bash, so each is a one-line shim
into this file (install.sh is a single apt-get line, since it has to install python3 and the compiler first).
Stdlib only, and Python 3.9 compatible: Ubuntu's python3 (or a Mac's) runs it, not the server's interpreter.

    python3 infra/ops.py build     # normal user: compile the pinned CPython into .python/, then .venv/ (see build())
    python3 infra/ops.py start     # foreground: exec main.py in .venv/. Env: SQLITE_PATH, JWT_SECRET, HOST, PORT

The rules want the entry built from source on the box, with no prebuilt binaries, so on Linux
build() compiles python.org's CPython against sqlite.org's SQLite, both pinned by SHA-256. The
server needs no third-party package: the venv only pins which interpreter start() runs, and has no pip.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The server's interpreter: CPython 3.15 compiled here with clang 19 (PGO + ThinLTO and the tail-call
# interpreter), linking SQLite 3.53 statically instead of Ubuntu's 3.45.
PYTHON_VERSION = "3.15.0"
PYTHON_URL = "https://www.python.org/ftp/python/{0}/Python-{0}.tar.xz".format(PYTHON_VERSION)
PYTHON_SHA256 = "ba4bed1ba346b916890b76d9e320451420aa69f6408997d33c66482eeae3d575"  # matches its .sigstore
SQLITE_VERSION = "3.53.1"
SQLITE_URL = "https://www.sqlite.org/2026/sqlite-autoconf-3530100.tar.gz"
SQLITE_SHA256 = "83e6b2020a034e9a7ad4a72feea59e1ad52f162e09cbd26735a3ffb98359fc4f"
# Ubuntu suffixes LLVM 19's tools (clang-19); configure looks for the plain names, which live in its own bin/.
LLVM_BIN = "/usr/lib/llvm-19/bin"
COMPILER = {"PATH": LLVM_BIN + os.pathsep + os.environ.get("PATH", ""), "CC": "clang", "AR": "llvm-ar",
            "LLVM_AR": "llvm-ar", "LLVM_PROFDATA": "llvm-profdata", "LDFLAGS": "-fuse-ld=lld"}

PYTHON_HOME = ROOT / ".python"
SOURCES = PYTHON_HOME / "src"
COMPILED = PYTHON_HOME / "install" / "bin" / "python3"
VENV = ROOT / ".venv"
VENV_PYTHON = VENV / "bin" / "python"
MINIMUM = (3, 14)  # PEP 695 syntax and deferred annotations (PEP 649)


def run(*command: str, cwd: Path = ROOT, env: dict = None) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=str(cwd), check=True, env=dict(os.environ, **(env or {})))


def fetch(url: str, sha256: str) -> Path:
    """Downloads url, checks its SHA-256 and unpacks it into .python/src/; returns the unpacked folder."""
    print("+ download", url, flush=True)
    archive = urllib.request.urlopen(url, timeout=300).read()
    if hashlib.sha256(archive).hexdigest() != sha256:
        raise SystemExit("the download does not match its pinned SHA-256: " + url)
    SOURCES.mkdir(parents=True, exist_ok=True)
    archive_path = SOURCES / url.rsplit("/", 1)[1]
    archive_path.write_bytes(archive)
    with tarfile.open(archive_path) as tar:
        top = tar.getnames()[0].split("/", 1)[0]
        if hasattr(tarfile, "tar_filter"):  # 3.12+: refuse paths outside .python/src/
            tar.extractall(SOURCES, filter="tar")
        else:
            tar.extractall(SOURCES)
    archive_path.unlink()
    return SOURCES / top


def compile_sqlite() -> Path:
    """SQLite's amalgamation as a static library; returns the folder with sqlite3.h and libsqlite3.a."""
    folder = fetch(SQLITE_URL, SQLITE_SHA256)
    run("clang", "-O2", "-fPIC", "-c", "sqlite3.c", "-o", "sqlite3.o", cwd=folder, env=COMPILER)
    run("llvm-ar", "rcs", "libsqlite3.a", "sqlite3.o", cwd=folder, env=COMPILER)
    return folder


def compile_python() -> str:
    """Compiles the pinned CPython into .python/install/ (once); returns its python3."""
    marker = PYTHON_HOME / ("built-" + PYTHON_VERSION + "-sqlite-" + SQLITE_VERSION)
    if marker.exists() and COMPILED.exists():
        return str(COMPILED)
    shutil.rmtree(str(PYTHON_HOME), ignore_errors=True)
    sqlite = compile_sqlite()
    source = fetch(PYTHON_URL, PYTHON_SHA256)
    env = dict(COMPILER, LIBSQLITE3_CFLAGS="-I" + str(sqlite), LIBSQLITE3_LIBS="-L" + str(sqlite) + " -lsqlite3 -lm")  # only the .a is there
    run("./configure", "--prefix=" + str(PYTHON_HOME / "install"), "--enable-optimizations", "--with-lto=thin",
        "--with-tail-call-interp", "--without-ensurepip", cwd=source, env=env)
    run("make", "-j" + str(os.cpu_count() or 1), cwd=source, env=env)
    run("make", "install", cwd=source, env=env)
    shutil.rmtree(str(SOURCES))
    marker.touch()
    return str(COMPILED)


def check_version(python: str) -> None:
    probe = "import sys, sqlite3; sys.exit(sys.version_info < %r or sqlite3.sqlite_version_info < (3, 44))" % (MINIMUM,)
    if subprocess.run([python, "-c", probe]).returncode != 0:
        raise SystemExit("%s is older than Python %d.%d (or its SQLite than 3.44); set PYTHON" % ((python,) + MINIMUM))


def build() -> None:
    """On Linux the compiled interpreter; elsewhere (or with PYTHON set) that one, if new enough. Then the venv."""
    if sys.platform == "linux" and not os.environ.get("PYTHON"):
        python = compile_python()
    else:
        python = os.environ.get("PYTHON") or sys.executable
    check_version(python)
    shutil.rmtree(str(VENV), ignore_errors=True)
    run(python, "-m", "venv", "--without-pip", str(VENV))
    print("+ server interpreter:", VENV_PYTHON, "->", python, flush=True)


def start() -> None:
    missing = [name for name in ("SQLITE_PATH", "JWT_SECRET", "HOST", "PORT") if not os.environ.get(name)]
    if missing:
        raise SystemExit("missing environment variables: " + ", ".join(missing))
    if not VENV_PYTHON.exists():
        raise SystemExit("no .venv/; run build.sh first")
    os.chdir(ROOT)
    os.execv(str(VENV_PYTHON), [str(VENV_PYTHON), str(ROOT / "main.py")])


COMMANDS = {"build": build, "start": start}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        raise SystemExit("usage: python3 infra/ops.py " + "|".join(COMMANDS))
    COMMANDS[sys.argv[1]]()
