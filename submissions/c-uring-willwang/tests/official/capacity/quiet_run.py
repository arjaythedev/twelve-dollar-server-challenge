"""Run a benchmark while excluding local component tests that use the same flock."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--trial", type=Path, required=True, help="new output directory made by the runner")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a benchmark command is required")
    with args.lock.open("a") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        if args.trial.exists():
            parser.error("use a fresh trial directory")
        start = time.time()
        rc = subprocess.run(command).returncode
        end = time.time()
        if args.trial.is_dir():
            stat = os.fstat(lock.fileno())
            data = {
                "lock_file": args.lock.name,
                "device": stat.st_dev,
                "inode": stat.st_ino,
                "acquired_unix": start,
                "finished_unix": end,
                "runner_exit": rc,
            }
            target = args.trial / "component-test-lock.json"
            temporary = target.with_suffix(".partial")
            temporary.write_text(json.dumps(data, indent=2) + "\n")
            temporary.replace(target)
    raise SystemExit(rc)


if __name__ == "__main__":
    main()
