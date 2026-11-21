import json
from statistics import median


def ramp_for(topology, users):
    if topology == "router-paced":
        return max(60, (3*users + 9999)//10000)
    return 60

def inspect(topology, entry, purpose, users, hold, tag):
    target = WORK / "runs" / tag
    ramp = ramp_for(topology,users)
    row = dict(entry=entry, topology=topology, purpose=purpose, tag=tag, users=users,
               ramp=ramp, hold=hold, summary_path=str(target / "summary.json"))
    summary = json.loads((target / "summary.json").read_text())
    score = json.loads((target / "score.json").read_text())
    if score["users"] != users or abs(score["duration_s"] - (ramp + hold)) > 2:
        raise ValueError(f"scorer duration/users differ for {tag}")
    actual = summary.get("max_open_clients_5s")
    client_files = sorted(target.glob("shard[0-9]*.json"))
    snapshots = {}
    for path in client_files:
        for tick in json.loads(path.read_text())["timeline"]:
            snapshots.setdefault(tick["t"],[]).append(tick["conns"])
    series = sorted((t,sum(v)) for t,v in snapshots.items() if len(v)==len(client_files))
    if actual is None:
        if not series:
            raise RuntimeError(f"missing client timelines for {tag}")
        actual = max(v for t,v in series)
    hold_connections = [v for t,v in series if ramp+10 <= t < ramp+hold]
    if hold_connections:
        row.update(min_open_after_ramp=min(hold_connections),median_open_after_ramp=median(hold_connections),
                   last_open_before_cutoff=hold_connections[-1],hold_fill_fraction=round(
                       sum(v>=users*.99 for v in hold_connections)/len(hold_connections),5))
    row.update(stock_pass=score["pass"], eligible=bool(score["pass"] and actual >= users*.99 and (not hold_connections or min(hold_connections)>=users*.99)),
               actual_open=actual, p95_ms=score["p95_ms"], p99_ms=score["p99_ms"],
               err_pct=score["err_pct"], req_per_s=score["req_per_s"])
    failure_file = target / "app_fail.json"
    if failure_file.is_file():
        row["app_failure"] = json.loads(failure_file.read_text())
        if row["app_failure"].get("elapsed_seconds",ramp+hold) < hold+ramp:
            row["eligible"] = False
            row["note"] = "server exited during scored interval"
    if score.get("errors"):
        row["errors"] = score["errors"]
    if score["pass"] and not row["eligible"] and not row.get("app_failure"):
        row["note"] = "stock pass with fewer than 99% of target connections observed"
    if (target / "close.json").exists():
        row["close"] = json.loads((target / "close.json").read_text())
    return row
