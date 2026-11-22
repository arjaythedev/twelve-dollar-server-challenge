#!/usr/bin/env python3
"""Head-to-head on this machine: this submission vs python-fastapi-cknutson12 (the merged FastAPI entry).

Both servers get identical k6 loads, interleaved so thermal drift hits both equally:
  stress    bench/stress.js, load.js's request mix without sleeps (64 users, 30 s): max req/s
  load2000  ../../bench/load.js at 2000 users: latency and server CPU at a realistic load
Server CPU per request is the number that carries over to the 1-vCPU box; req/s on a many-core
Mac flatters Granian, whose Rust I/O thread runs in parallel with Python here.

    python3 bench/compare.py [rounds]    # needs k6 and a Python >= 3.12 in $PYTHON
"""

import json, os, resource, shutil, signal, subprocess, sys, tempfile, time, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parent.parent
WORK = ROOT / "build" / "compare"
SUBS = ["python-fastapi-cknutson12", ROOT.name]
PY = os.environ.get("PYTHON") or "python3"
BASE = "http://127.0.0.1:3000"


def cpu_seconds(pgid: int) -> float:
    out = subprocess.run(["ps", "-A", "-o", "pgid=,time="], capture_output=True, text=True).stdout
    total = 0.0
    for line in out.splitlines():
        g, t = line.split()
        if int(g) == pgid:
            parts = [float(x) for x in t.replace("-", ":").split(":")]
            secs = 0.0
            for p in parts:
                secs = secs * 60 + p
            total += secs
    return total


def k6(script: Path, summary: Path, **env: str) -> dict:
    args = ["k6", "run", "-q", "--summary-export", str(summary)]
    for k, v in env.items():
        args += ["-e", f"{k}={v}"]
    subprocess.run(args + [str(script)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    m = json.loads(summary.read_text())["metrics"]
    d = m["http_req_duration"]
    return {"reqs": m["http_reqs"]["count"], "rps": m["http_reqs"]["rate"],
            "failed": m["http_req_failed"]["value"], "p95": d["p(95)"], "p99": d["p(99)"]}


def run(sub: str, phase: str, script: Path, **env: str) -> dict:
    tmp = Path(tempfile.mkdtemp(dir=WORK))
    db = tmp / "feed.db"
    shutil.copy(REPO / "seed" / "feed.db", db)
    senv = dict(os.environ, SQLITE_PATH=str(db), JWT_SECRET="twelve-dollar-challenge", HOST="127.0.0.1", PORT="3000")
    log = open(tmp / "server.log", "w")
    server = subprocess.Popen(["bash", str(REPO / "submissions" / sub / "start.sh")], env=senv,
                              stdout=log, stderr=log, start_new_session=True)
    try:
        for _ in range(120):
            try:
                urllib.request.urlopen(BASE + "/health", timeout=1)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise SystemExit(f"{sub} never became healthy; see {tmp / 'server.log'}")
        time.sleep(1)
        cpu0 = cpu_seconds(server.pid)
        result = k6(script, tmp / "summary.json", BASE_URL=BASE, TOKENS=str(REPO / "seed" / "tokens.json"), **env)
        result["cpu_s"] = cpu_seconds(server.pid) - cpu0
        result["cpu_us_per_req"] = result["cpu_s"] / result["reqs"] * 1e6
    finally:
        os.killpg(server.pid, signal.SIGINT)
        try:
            server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(server.pid, signal.SIGKILL)
        time.sleep(2)
    print(f"{phase:9} {sub:40} " + json.dumps({k: round(v, 3) for k, v in result.items()}), flush=True)
    return result


def main() -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_NOFILE, (65535, resource.RLIM_INFINITY))
    shim = WORK / "bin"
    shim.mkdir(exist_ok=True)
    (shim / "python3").unlink(missing_ok=True)
    (shim / "python3").symlink_to(shutil.which(PY) or PY)
    os.environ["PATH"] = f"{shim}:{os.environ['PATH']}"  # cknutson12's build.sh calls bare python3
    for sub in SUBS:
        subprocess.run(["bash", str(REPO / "submissions" / sub / "build.sh")], check=True, env=dict(os.environ, PYTHON=PY),
                       stdout=subprocess.DEVNULL)

    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    results = {s: {"stress": [], "load": []} for s in SUBS}
    for _ in range(rounds):  # interleave so drift (thermals, background load) hits both equally
        for sub in SUBS:
            results[sub]["stress"].append(run(sub, "stress", HERE / "stress.js", VUS="64", DURATION="30s"))
    for sub in SUBS:
        results[sub]["load"].append(run(sub, "load2000", REPO / "bench" / "load.js", VUS="2000", DURATION="60s", RAMP_UP="20s"))
    (WORK / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
