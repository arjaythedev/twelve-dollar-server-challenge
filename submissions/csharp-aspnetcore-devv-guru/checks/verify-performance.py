#!/usr/bin/env python3
"""Verify a stopped local-performance run against its acknowledged create count.

Run in WSL/Linux with the same seed used for the load test. Databases are opened
read-only. This checks consistency after load, not machine power-loss durability.
"""
import argparse
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path, help="One VU result directory containing metadata.json")
parser.add_argument("--seed", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
metadata = json.loads((args.directory / "metadata.json").read_text())
state = subprocess.run(["systemctl", "is-active", "--", metadata["unit"]], capture_output=True, text=True)
assert state.stdout.strip() in ("inactive", "failed", "unknown"), "Stop the measured server before checking its database"

acknowledged = {}
for label in ("warmup", "measured"):
    path = args.directory / f"{label}-summary.json"
    if path.is_file():
        metrics = json.loads(path.read_text())["metrics"]
        created = metrics["http_reqs{name:POST /posts}"]["values"]["count"]
        failed = metrics["http_req_failed{name:POST /posts}"]["values"]["passes"]
        acknowledged[label] = int(created - failed)

with closing(sqlite3.connect(args.seed.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
    seed_posts = connection.execute("SELECT count(*) FROM posts").fetchone()[0]
database = Path(metadata["database"])
with closing(sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
    check = connection.execute("PRAGMA quick_check").fetchall()
    violations = connection.execute("PRAGMA foreign_key_check").fetchall()
    posts = connection.execute("SELECT count(*) FROM posts").fetchone()[0]
    likes = connection.execute("SELECT count(*) FROM likes").fetchone()[0]
assert check == [("ok",)], check
assert not violations, violations[:10]
assert posts == seed_posts + sum(acknowledged.values()), (posts, seed_posts, acknowledged)
result = {"run": str(args.directory), "checked_after_server_shutdown": True,
          "quick_check": "ok", "foreign_key_violations": 0,
          "seed_posts": seed_posts, "acknowledged_creates": acknowledged,
          "post_count": posts, "like_count": likes, "post_count_matches_acknowledged_creates": True,
          "scope": "Post-load consistency; not a power-failure test. An acknowledged create is a create request minus HTTP failures; separate integration tests enforce the 201 contract."}
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
