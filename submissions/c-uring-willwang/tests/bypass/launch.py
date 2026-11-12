"""Enter the scoring resource budget before touching SQLite or mapping the packet rings."""

import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from network import receive, STACK_IP

cfg = json.loads(Path(sys.argv[1]).read_text())
os.sched_setaffinity(0, {cfg["cpu"]})
resource.setrlimit(resource.RLIMIT_NOFILE, (65535, 65535))
status = dict(
    line.split(":", 1) for line in Path("/proc/self/status").read_text().splitlines() if ":" in line
)
if os.geteuid() == 0 or int(status["CapEff"], 16) or int(status["CapPrm"], 16):
    raise RuntimeError("the measured native process must be an ordinary user with zero capabilities")
if cfg.get("source"):
    shutil.copyfile(cfg["source"], cfg["db"])
    with open(cfg["db"], "rb") as db:
        digest = hashlib.file_digest(db, "sha256").hexdigest()
    if digest != cfg["seed_sha256"]:
        raise RuntimeError("the physical seed copy differs")
control, fd, metadata = receive(cfg["broker"])
control.set_inheritable(True)
env = dict(
    os.environ, HOST="0.0.0.0", PORT=str(cfg["port"]), SQLITE_PATH=cfg["db"], JWT_SECRET=cfg["secret"]
)
args = [cfg["binary"]]
if cfg["mode"] == "packet":
    os.set_inheritable(fd, True)
    args += [
        "--interface",
        "bench0",
        "--address",
        STACK_IP + "/24",
        "--mode",
        "packet",
        "--packet-fd",
        str(fd),
    ]
    if cfg.get("packet_rx_frames"):
        args += ["--packet-rx-frames", str(cfg["packet_rx_frames"])]
    if cfg.get("packet_tx_frames"):
        args += ["--packet-tx-frames", str(cfg["packet_tx_frames"])]
    if cfg.get("packet_gso"):
        args.append("--packet-gso")
else:
    os.close(fd)
    env.update(
        LD_PRELOAD=cfg["preload"], TWELVE_TEST_SOCKET_ROOT=cfg["network"], TWELVE_TEST_SOCKET_COUNT="1"
    )
runtime = Path(cfg["runtime"])
temporary = runtime.with_suffix(".pending")
temporary.write_text(json.dumps({"pid": os.getpid(), "link": metadata}, indent=2) + "\n")
temporary.replace(runtime)
os.execve(args[0], args, env)
