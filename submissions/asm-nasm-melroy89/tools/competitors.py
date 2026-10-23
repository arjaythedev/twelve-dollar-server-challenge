#!/usr/bin/env python3
"""Capture the complete upstream PR field and immutable heads. Read-only remote access."""
import argparse
import datetime
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO = "arjaythedev/twelve-dollar-server-challenge"
p = argparse.ArgumentParser()
p.add_argument("--output", type=pathlib.Path, default=ROOT / "results/competitors.json")
args = p.parse_args()

def api(endpoint):
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))
pulls = []
page = 1
while True:
    items = api(f"repos/{REPO}/pulls?state=all&per_page=100&page={page}")
    pulls.extend(items)
    if len(items) < 100:
        break
    page += 1
inventory = []
for pr in pulls:
    entry = {
        "number": pr["number"], "title": pr["title"], "state": pr["state"],
        "url": pr["html_url"], "head_sha": pr["head"]["sha"],
        "head_repository": pr["head"]["repo"]["full_name"] if pr["head"]["repo"] else None,
        "body": pr["body"],
        "eligibility": "not independently screened",
        "build": "not run", "official_tests": "not run", "comparable_capacity": "not run",
    }
    inventory.append(entry)
record = {
    "retrieved_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "repository": REPO, "entries": inventory,
    "boundary": "Inventory only. Titles, descriptions and unlike benchmark results do not establish eligibility or ranking."
}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps({"entries": len(inventory), "output": str(args.output)}))
