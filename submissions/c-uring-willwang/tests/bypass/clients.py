"""Unchanged organizer Go processes in a single isolated Linux client network namespace."""

from concurrent.futures import ThreadPoolExecutor
import json
from math import ceil
import subprocess
import time

from common import collect_dir, docker, group_counts, group_of
from network import CLIENT_IP


def prepare(args, link):
    cpus = link.groups[0]
    count = args.shards or ceil(args.users / 12_500)
    if not 1 <= count <= min(64, args.users):
        raise ValueError("the shard count must be between one and the smaller of 64 and the user count")
    if count == 1:
        if args.users > 28_000:
            raise ValueError("select enough genuine sources for the requested users")
        routes = [{"uid": 65532, "address": CLIENT_IP}]
    else:
        from sources import provision

        routes = provision(link, count)
        if ceil(args.users / count) > link.source_limits["available_per_address"]:
            raise ValueError("the number of requested users exceeds the configured source port pool")
    # Separate Go workers share small disjoint CPU groups, as on distributed load generators.
    width = len(cpus) if count == 1 else min(3, len(cpus))
    groups = [cpus[i : i + width] for i in range(0, len(cpus), width)]
    workers = []
    for i, route in enumerate(routes):
        begin, end = args.users * i // count, args.users * (i + 1) // count
        output = link.reports[0] if i == 0 else collect_dir(args.out, f"go{i}")
        workers.append(
            dict(
                route,
                index=i,
                offset=begin,
                users=end - begin,
                cpus=groups[i % len(groups)],
                output=output,
                only=count == 1,
            )
        )
    return workers


def launch(args, link, worker, target, start, port):
    cpus = worker["cpus"]
    command = [
        "/hold",
        "/mload",
        "-target",
        target,
        "-ports",
        str(port),
        "-tokens",
        "/tokens.json",
        "-users",
        str(worker["users"]),
        "-offset",
        str(worker["offset"]),
        "-ramp",
        f"{args.ramp}s",
        "-hold",
        f"{args.hold}s",
        "-think-scale",
        str(args.think),
        "-timeout",
        "60s",
        "-start",
        str(start),
        "-out",
        "/out/score.json",
    ]
    if worker["only"]:
        container = link.client_ids[0]
        docker("exec", "-d", "--env", f"GOMAXPROCS={len(cpus)}", container, *command)
    else:
        name = f"go{worker['index']}"
        # Register the deterministic owned name before creation in case setup is interrupted.
        container = link.unique + "-" + name
        link.containers.append(container)
        limit = 160 + ceil(worker["users"] * 48 / 1024)
        uid = worker["uid"]
        docker(
            "run",
            "-d",
            *link.base(name),
            "--network",
            "container:" + link.client_ids[0],
            "--user",
            f"{uid}:{uid}",
            "--memory",
            f"{limit}m",
            "--memory-swap",
            f"{limit}m",
            "--pids-limit",
            "100",
            "--cpuset-cpus",
            ",".join(map(str, cpus)),
            "--ulimit",
            "nofile=131072:131072",
            "--env",
            f"GOMAXPROCS={len(cpus)}",
            "--mount",
            f"type=bind,src={worker['output']},dst=/out",
            "--entrypoint",
            "/hold",
            link.image,
            *command[1:],
        )
    worker["cgroup"] = group_of(int(docker("inspect", "--format", "{{.State.Pid}}", container)))


def start_workers(args, link, workers, target, port):
    start = time.time_ns() // 1_000_000 + (2500 if len(workers) == 1 else 6500 + 700 * len(workers))
    with ThreadPoolExecutor(max_workers=min(len(workers), 8)) as pool:
        list(pool.map(lambda worker: launch(args, link, worker, target, start, port), workers))
    if time.time_ns() // 1_000_000 >= start:
        raise RuntimeError("a load generator missed its common start time")
    return start


def counters(workers):
    per_shard = [group_counts(w["cgroup"]) for w in workers]
    if any("usage_usec" not in row for row in per_shard):
        raise RuntimeError("a load generator's resource group vanished")
    return {key: sum(r[key] for r in per_shard) for key in per_shard[0]}


def read_results(args, workers):
    deadline = time.monotonic() + 65
    paths = [w["output"] for w in workers]
    while not all((d / "exit").exists() for d in paths) and time.monotonic() < deadline:
        time.sleep(0.05)
    if not all((d / "exit").exists() and (d / "exit").read_text().strip() == "0" for d in paths):
        raise RuntimeError("an organizer load generator did not finish normally")
    inputs = [path / "score.json" for path in paths]
    merged = args.out / "score.json"
    with (args.out / "merge.log").open("w") as log:
        subprocess.run(
            [args.work / "mload", "-merge", "-out", merged, *inputs],
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    shard_reports = [json.loads(path.read_text()) for path in inputs]
    reports_by_time = [{row["t"]: row for row in report.get("timeline", [])} for report in shard_reports]
    aligned = sorted(set.intersection(*(set(t) for t in reports_by_time)))
    ticks = [
        {"t": t, **{key: sum(shard[t][key] for shard in reports_by_time) for key in ("conns", "reqs")}}
        for t in aligned
    ]
    return json.loads(merged.read_text()), ticks
