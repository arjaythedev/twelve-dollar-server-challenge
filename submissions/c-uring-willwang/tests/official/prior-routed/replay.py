#!/usr/bin/env python3
"""Run one scheduled historical single-router trial with signed network and scope evidence."""
import argparse
import ast
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import runpy
import subprocess
import sys
import time
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from observe import (
    add_placement_checks,
    dump,
    occupancy,
    service_snapshot,
    wire_evidence,
)
import archived_score
import directional
from assess_window import assess
from observe import timed_qdisc_evidence

HISTORICAL_INPUTS = {
    "feed.db": "102193bcdb4ced52abb98f6bdfb19e06284273af30b74cccd009b257f09569cc",
    "tokens.json": "7c4eb45a67156ab3827d6fadab40c884fcab77d4bc4051bffe9e537a16fffa50",
}
PROFILE = "historical-single-middle-router-paced"
PROFILES = {"priorroute": PROFILE, "priorrps": directional.PROFILE}
SHARDS = [{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {11, 12, 13}]
STOCK = "48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6"
ARCHIVED_MANIFEST = "0c8c6e7fb2c565df2d83cce865e7ddaf4310fc2c2e4b4ff207a4ec1c2a853500"


def read(path):
    return json.loads(path.read_text())


def sha(path):
    with path.open("rb") as src:
        return hashlib.file_digest(src, "sha256").hexdigest()


def sources(router=None):
    receipt = HERE / "source/hashes.json"
    if sha(receipt) != ARCHIVED_MANIFEST:
        raise ValueError("Independent original SHA-256 manifest changed")
    proof = read(receipt)
    if len(proof) != 7 or not all(
        v["verified"] and (router is None or sha(router / k) == v["expected"])
        for k, v in proof.items()
    ):
        raise ValueError(
            "Historical source or placement binary does not match the seven-file archive"
        )
    return {k: v["expected"] for k, v in proof.items()}


def ramp_for(users):
    return max(60, (3 * users + 9999) // 10000)


def arguments(args, work):
    return [
        str(args.work / "recovered/router/eth_run.py"),
        "--work",
        str(work),
        str(args.server),
        args.tag,
        "--users",
        str(args.users),
        "--ramp",
        str(ramp_for(args.users)),
        "--hold",
        str(args.hold),
        "--source",
        str(args.seed),
        "--tokens",
        str(args.tokens),
    ]


def test_dry(args):
    # Use the original function for the schedule and original stock argv expression;
    # the factual dry receipt alone makes no claim about runtime packet placement.
    assert archived_score.ramp_for("router-paced", args.users) == ramp_for(args.users)
    router = args.work / "recovered/router"
    tree = ast.parse((router / "eth_run.py").read_text())
    found = [
        n
        for n in ast.walk(tree)
        if isinstance(n, ast.List)
        and any(
            isinstance(x, ast.Constant) and x.value == "-think-scale" for x in n.elts
        )
    ]
    assert len(found) == 1
    expr = compile(ast.Expression(found[0]), "archived-stock-argv", "eval")
    descs = []
    for i, group in enumerate(SHARDS):
        left, right = (args.users * i // 4, args.users * (i + 1) // 4)
        stock_args = SimpleNamespace(
            program=args.mload,
            port=19432,
            ramp=ramp_for(args.users),
            hold=args.hold,
            tokens=args.tokens,
        )
        desc = {
            "users": right - left,
            "offset": left,
            "targets": [f"10.240.{i+1}.{j}" for j in range(1, 13)],
        }
        cmd = eval(
            expr,
            {
                "args": stock_args,
                "desc": desc,
                "start_ms": 1700000000000,
                "result_path": f"shard{i}.json",
            },
        )
        descs.append(
            {
                "cpus": sorted(group),
                "source": f"10.243.{i+1}.1",
                "users": right - left,
                "offset": left,
                "stock_argv_with_example_common_start": cmd,
            }
        )
    work = args.output / args.tag / "work"
    manifest = sources(router)
    return {
        "profile": PROFILES[args.profile],
        **({"directional_rps": directional.CONFIG} if args.profile == "priorrps" else {}),
        "tag": args.tag,
        "command": arguments(args, work),
        "original_seven": manifest,
        "mload_sha256": sha(args.mload),
        "server_sha256": sha(args.server),
        "sources_checked": len(manifest),
        "four_stock_workers": descs,
        "native_working_directory_for_service": "raw result directory (archived runner)",
        "dry_run": True,
        "sockets_opened": 0,
        "namespaces_created": 0,
    }


def identity():
    if os.geteuid() == 0:
        raise ValueError("Run as the ordinary benchmark user with its systemd manager")
    return pwd.getpwuid(os.geteuid())


def setup_work(path, args):
    for p in [path, path / "work", path / "work/runs"]:
        p.mkdir()
    (path / "raw").symlink_to(Path("work/runs") / args.tag, target_is_directory=True)
    (path / "work/mload").symlink_to(args.mload)
    (path / "work/mload.sha256").write_text(STOCK + "\n")
    if args.mload_build_info:
        info = args.mload_build_info.read_text()
    else:
        info = subprocess.check_output(["go", "version", "-m", args.mload], text=True)
    (path / "work/mload.build.txt").write_text(info)


def worker(args, directory):
    # The controller's observer is separate. The archived runner starts with all 16
    # fixture CPUs and moves its manager to the spare CPU before the stock clock.
    os.sched_setaffinity(0, set(range(16)))
    if os.geteuid() == 0:
        raise RuntimeError("archived runner must be unprivileged")
    router = args.work / "recovered/router"
    sys.path.insert(0, str(router))
    import private_net

    add_placement_checks(private_net, directory)
    if args.profile == "priorrps":
        directional.install(private_net, directory)
    sys.argv = arguments(args, directory / "work")
    runpy.run_path(str(router / "eth_run.py"), run_name="__main__")


def watch(proc, out):
    raw = out / "raw"
    next_audit = 0
    cutoff = None
    read_phases = set()
    with (out / "charged-descendants.jsonl").open("w") as fp:
        while proc.poll() is None:
            now = time.monotonic()
            if now >= next_audit and (raw / "runtime.json").is_file():
                try:
                    runtime = read(raw / "runtime.json")
                    if cutoff is None:
                        plan = read(raw / "plan.json")
                        cutoff = (
                            runtime["start_unix_ms"] / 1000
                            + plan["ramp_s"]
                            + plan["hold_s"]
                        )
                    snap = service_snapshot(runtime)
                    print(json.dumps(snap, separators=(",", ":")), file=fp, flush=True)
                except (FileNotFoundError, ProcessLookupError, json.JSONDecodeError):
                    pass
                next_audit = now + 5
            wall = time.time()
            phase = (
                "before"
                if cutoff is not None and cutoff - 1.5 <= wall < cutoff - 0.5
                else (
                    "after"
                    if cutoff is not None and cutoff <= wall < cutoff + 2
                    else None
                )
            )
            if phase is not None and phase not in read_phases:
                try:
                    timed_qdisc_evidence(subprocess, out, phase)
                except (
                    FileNotFoundError,
                    ProcessLookupError,
                    PermissionError,
                    subprocess.SubprocessError,
                ) as exc:
                    dump(
                        out / f"qdisc-{phase}-observer-error.json",
                        {"error": str(exc), "unix": time.time()},
                    )
                read_phases.add(phase)
            time.sleep(0.05 if cutoff is not None and abs(cutoff - wall) <= 2 else 0.25)
    return proc.returncode


def build_summary(args, out, rc):
    if getattr(args, "profile", "priorroute") == "priorrps":
        directional.verify(out)
    raw = out / "raw"
    plan = read(raw / "plan.json")
    original = read(raw / "summary.json")
    merged = read(raw / "score.json")
    old = archived_score
    old.WORK = out / "work"
    archived = old.inspect(
        "router-paced",
        Path(args.server).name,
        "replay",
        args.users,
        args.hold,
        args.tag,
    )
    occ = occupancy(raw, args)
    proof = read(out / "placements.json")
    samples = [json.loads(x) for x in (raw / "samples.jsonl").read_text().splitlines()]
    wire = wire_evidence(
        samples, plan, [read(raw / f"shard{i}.json") for i in range(4)]
    )
    dump(out / "scored-wire-cpu.json", wire)
    review = assess(out, plan, original, merged, occ, wire, rc)
    dump(out / "scored-assessment.json", review)
    dump(out / "synchronized-connections.json", occ)
    dump(out / "historical-assessment.json", archived)
    data = {
        **merged,
        "tag": args.tag,
        "stock_pass": bool(merged["pass"]),
        "qualified": review["qualified"],
        "historical_eligible": archived["eligible"],
        "infrastructure_flags": review["flags"],
        "full_presence": occ["simultaneous_all_shards"],
        "app_oom": review["scored_oom"],
        "app_failure": archived.get("app_failure"),
        "scored_app_state": review["lifetime"]["state"],
        "service_scope_samples": review["service_scope_samples"],
        "private_hash_by_role": {
            k: v["ipv4/tcp_ehash_entries"] for k, v in proof["namespaces"].items()
        },
        "hold_server_cpu": original["hold_server_cpu"],
        "whole_run_server_cpu": original["whole_run_server_cpu"],
        "original_summary": "raw/summary.json",
        "diagnostic_profile": True,
    }
    # The original full stock JSON/histograms stay in raw; series only needs its scalar result.
    for k in ("hist", "connect_hist", "endpoints", "timeline"):
        data.pop(k, None)
    dump(out / "summary.json", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="priorroute")
    for name in [
        "server",
        "cwd",
        "seed",
        "tokens",
        "mload",
        "output",
        "work",
        "component-lock",
        "serial-lock",
    ]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument(
        "--mload-build-info",
        type=Path,
        help="optional existing go version -m record for the verified mload",
    )
    parser.add_argument("--users", type=int, required=True)
    parser.add_argument("--hold", type=int, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument(
        "--input-profile",
        choices=["common", "historical-exact"],
        default="common",
        help="validate both historical inputs for separate priorroute-exact-* tags",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--internal-worker", action="store_true", help=argparse.SUPPRESS
    )
    args = parser.parse_args()
    for key in [
        "server",
        "cwd",
        "seed",
        "tokens",
        "mload",
        "output",
        "work",
        "component_lock",
        "serial_lock",
        "mload_build_info",
    ]:
        if getattr(args, key) is None:
            continue
        setattr(args, key, getattr(args, key).resolve())
    if (
        not re.fullmatch(re.escape(args.profile) + r"-[A-Za-z0-9_.-]+", args.tag)
        or not (4 <= args.users <= 500000)
        or args.hold <= 0
    ):
        parser.error("invalid tag for the selected profile, user count or duration")
    if args.profile == "priorrps" and (
        args.input_profile != "common" or args.tag.startswith("priorrps-exact-")
    ):
        parser.error("the directional comparison uses its separately named common-input control")
    if (args.input_profile == "historical-exact") != args.tag.startswith(
        "priorroute-exact-"
    ):
        parser.error(
            "historical exact-input controls require their own priorroute-exact-* tag"
        )
    if args.dry_run:
        print(json.dumps(test_dry(args), indent=2))
        return 0
    out = args.output / args.tag
    if args.internal_worker:
        worker(args, out)
        return 0
    if os.sched_getaffinity(0) != {15}:
        os.sched_setaffinity(0, {15})
    original = sources(args.work / "recovered/router")
    if sha(args.mload) != STOCK or args.component_lock == args.serial_lock:
        parser.error(
            "supply the archived stock scorer and two distinct shared lock paths"
        )
    user = identity()
    args.output.mkdir(exist_ok=True, parents=True)
    with contextlib.ExitStack() as stack:
        locked = []
        for p in [args.component_lock, args.serial_lock]:
            fd = stack.enter_context(p.open("a"))
            fcntl.flock(fd, fcntl.LOCK_EX)
            locked.append((p, os.fstat(fd.fileno()), time.time()))
        setup_work(out, args)
        seed_sha = sha(args.seed)
        tokens_sha = sha(args.tokens)
        if args.input_profile == "historical-exact":
            if {"feed.db": seed_sha, "tokens.json": tokens_sha} != HISTORICAL_INPUTS:
                raise ValueError(
                    "Historical archive seed or tokens differ before network setup"
                )
        shape = {
            "topology": PROFILES[args.profile],
            "tag": args.tag,
            "users": args.users,
            "ramp_s": ramp_for(args.users),
            "hold_s": args.hold,
            "server": str(args.server),
            "server_sha256": sha(args.server),
            "seed": str(args.seed),
            "tokens": str(args.tokens),
            "seed_sha256": seed_sha,
            "tokens_sha256": tokens_sha,
            "input_profile": args.input_profile,
            "mload_sha256": STOCK,
            "original_seven": original,
            "cpu9_lock": str(locked[0][0]),
            "serial_lock": str(locked[1][0]),
            "owner_uid": user.pw_uid,
            "external_fixture_source": str(HERE),
            "adapter_sha256": {
                f: sha(HERE / f)
                for f in (
                    "replay.py",
                    "observe.py",
                    "collect.py",
                    "archived_score.py",
                    "assess_window.py",
                )
            },
        }
        if args.profile == "priorrps":
            shape["adapter_sha256"]["directional.py"] = sha(HERE / "directional.py")
        dump(out / "shape.json", shape)
        env = {
            **os.environ,
            "USER": user.pw_name,
            "LOGNAME": user.pw_name,
            "HOME": user.pw_dir,
            "XDG_RUNTIME_DIR": f"/run/user/{user.pw_uid}",
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{user.pw_uid}/bus",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        cmd = [sys.executable, str(HERE / "replay.py"), "--internal-worker"]
        for key in (
            "profile",
            "server",
            "cwd",
            "seed",
            "tokens",
            "mload",
            "output",
            "work",
            "component_lock",
            "serial_lock",
            "users",
            "hold",
            "tag",
            "input_profile",
            "mload_build_info",
        ):
            value = getattr(args, key)
            if value is not None:
                cmd.extend(("--" + key.replace("_", "-"), str(value)))
        rc = 1
        try:
            with (out / "driver.log").open("w") as log:
                proc = subprocess.Popen(
                    cmd, cwd=args.cwd, env=env, stdout=log, stderr=subprocess.STDOUT
                )
                try:
                    rc = watch(proc, out)
                finally:
                    if proc.poll() is None:
                        proc.terminate()
                        proc.wait()
            if (out / "raw/summary.json").exists() and (
                out / "raw/score.json"
            ).exists():
                build_summary(args, out, rc)
            dump(
                out / "dispatch-status.json",
                {
                    "worker_exit": rc,
                    "stock_present": (out / "raw/score.json").is_file(),
                    "completed": (out / "summary.json").is_file(),
                    "raw": "raw",
                    "log": "driver.log",
                },
            )
        finally:
            for (p, s, acquired), name in zip(
                locked, ["component-test-lock.json", "serial-lock.json"]
            ):
                dump(
                    out / name,
                    {
                        "lock_file": p.name,
                        "device": s.st_dev,
                        "inode": s.st_ino,
                        "acquired_unix": acquired,
                        "finished_unix": time.time(),
                        "runner_exit": rc,
                    },
                )
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
