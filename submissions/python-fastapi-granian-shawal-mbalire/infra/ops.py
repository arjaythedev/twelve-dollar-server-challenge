#!/usr/bin/env python3
"""The challenge's build / start steps, in Python.

build.sh and start.sh are required by the challenge and run with bash, so each is a one-line shim
into this file (install.sh is a single apt-get line, since it has to install python3 first).
Stdlib only, and Python 3.9 compatible: it runs before the venv exists.

    python3 infra/ops.py build     # normal user: .venv with the pinned requirements (PYTHON picks the interpreter)
    python3 infra/ops.py start     # foreground: exec Granian. Env: SQLITE_PATH, JWT_SECRET, HOST, PORT
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# One worker on a single-threaded runtime: the box has 1 vCPU and SQLite has one connection.
GRANIAN_FLAGS = [
    "--interface", "asgi",  # asgi, not asginl: lifespan opens/closes the store and the profiler
    "--workers", "1",
    "--runtime-mode", "st",
    "--loop", "uvloop",
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


def build() -> None:
    python = os.environ.get("PYTHON") or sys.executable
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
