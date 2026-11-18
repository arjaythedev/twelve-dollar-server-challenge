#!/usr/bin/env bash
# benchmarks/compare.sh: compare two JSON benchmark reports (before/after).
set -euo pipefail
BEFORE="${1:?usage: compare.sh before.json after.json}"
AFTER="${2:?usage: compare.sh before.json after.json}"
python3 - "$BEFORE" "$AFTER" <<'PY'
import json, sys
b = json.load(open(sys.argv[1]))
a = json.load(open(sys.argv[2]))
def t(r):
    return r.get("totals", {})
bt, at = t(b), t(a)
print(f"{'metric':<28} {'before':>12} {'after':>12} {'delta':>10}")
for k in ["http_reqs","http_req_failed","iterations","vus_max"]:
    bv = bt.get(k, 0); av = at.get(k, 0)
    d = av - bv
    print(f"{k:<28} {bv:>12} {av:>12} {d:>+10}")
for k in ["http_req_duration"]:
    bd = bt.get(k, {}); ad = at.get(k, {})
    for p in ["avg","med","p(95)","p(99)","max"]:
        bv = bd.get(p, 0); av = ad.get(p, 0)
        print(f"{k}.{p:<22} {bv:>12.2f} {av:>12.2f} {av-bv:>+10.2f}")
PY
