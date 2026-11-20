"""Start the ordinary native binary inside the scored scope after charging its seeded database."""

import hashlib, json, os, resource, shutil, sys
from pathlib import Path

cfg = json.loads(Path(sys.argv[1]).read_text())
os.sched_setaffinity(0, {cfg["cpu"]})
resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))
shutil.copyfile(cfg["seed"], cfg["db"])
with open(cfg["db"], "rb") as seed:
    Path(cfg["root"], "seed.verified").write_text(hashlib.file_digest(seed, "sha256").hexdigest())
Path(cfg["pid"]).write_text(str(os.getpid()))
env = dict(
    os.environ,
    HOST="0.0.0.0",
    PORT="19432",
    JWT_SECRET=cfg["secret"],
    SQLITE_PATH=cfg["db"],
    LD_PRELOAD=cfg["preload"],
    TWELVE_TEST_SOCKET_ROOT=cfg["root"],
    TWELVE_TEST_SOCKET_COUNT=str(cfg["listeners"]),
)
os.execve(cfg["server"], [cfg["server"]], env)
