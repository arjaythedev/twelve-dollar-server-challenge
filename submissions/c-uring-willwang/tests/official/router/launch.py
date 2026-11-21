"""Charge the fresh SQLite copy to the app cgroup and exec the unprivileged server."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

out, seed, server = map(Path, sys.argv[1:4])
db = out / "feed.db"
shutil.copyfile(seed, db)
with db.open("rb") as stream:
    seed_sha = hashlib.file_digest(stream, "sha256").hexdigest()
(out / "app-start.json").write_text(
    json.dumps(
        {
            "uid": os.getuid(),
            "seed_sha256": seed_sha,
            "pid": os.getpid(),
            "netns": os.readlink("/proc/self/ns/net"),
            "affinity": sorted(os.sched_getaffinity(0)),
            "cgroup": Path("/proc/self/cgroup").read_text().strip(),
        }
    )
)
os.environ["SQLITE_PATH"] = str(db)
os.execv(str(server), [str(server)])
