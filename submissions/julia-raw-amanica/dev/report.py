"""Rewrites the progress section of README.md (between the results markers) from stress-results.csv:
one line per change (LABEL, in the order first measured) with its best local numbers and the change
against the previous step. Labels starting with "probe" or "smoke" are measurements, not changes: skipped.

usage: python3 -I dev/report.py <csv> <README.md>
"""
import csv, sys

rows = list(csv.DictReader(open(sys.argv[1])))
steps = []                                   # labels in order of first appearance
best = {}                                    # label -> {"k6": users, "users": users, "blast": req/s}
blast_runs = {}
for r in rows:
    lab = r["label"]
    if lab.lower().startswith(("probe", "smoke")):
        continue
    if lab not in best:
        steps.append(lab)
        best[lab] = {"k6": None, "blast": None, "users": None}
    if r["pass"] != "1":
        continue
    gen = r.get("gen") or "k6"
    if gen == "blast":                       # blast varies ~10% run to run: keep all, take the median
        blast_runs.setdefault(lab, []).append(float(r["req_s"]))
        continue
    v = int(r["vus"])
    if best[lab][gen] is None or v > best[lab][gen] or (v == best[lab][gen] and gen == "users"):
        best[lab][gen] = v
        if gen == "users":
            best[lab]["rss"] = r.get("srv_rss_peak_mb") or ""
for lab, xs in blast_runs.items():
    xs.sort()
    best[lab]["blast"] = xs[len(xs) // 2] if len(xs) % 2 else (xs[len(xs) // 2 - 1] + xs[len(xs) // 2]) / 2
    best[lab]["nblast"] = len(xs)


def delta(cur, prev):
    if cur is None or prev is None:
        return ""
    return f" ({100 * (cur - prev) / prev:+.0f}%)"


L = ["Best local result per change. Raw runs: [stress-results.csv](stress-results.csv); how to run: above.",
     "Blast req/s (max throughput at 1 CPU): median of the passing runs (they vary ~10%), with % change against the",
     "previous change and the number of runs. Changes under ~10% are noise unless they repeat.",
     "k6 / wrk users = highest passing run (not always a full ladder). k6 is capped near 20k users on this host;",
     "wrk users (`GEN=users`, think time per connection) are cheap, so they reach the connection limits.", "",
     "| change | k6 users | wrk users (server RSS) | blast req/s |", "|---|--:|--:|--:|"]
prev = {"k6": None, "blast": None, "users": None}
for lab in steps:
    b = best[lab]
    k6 = f"{b['k6']:,}" if b["k6"] else "–"
    bl = f"{b['blast']:,.0f}{delta(b['blast'], prev['blast'])}, n={b.get('nblast', 1)}" if b["blast"] else "–"
    us = f"{b['users']:,} ({b.get('rss') or '?'} MB)" if b["users"] else "–"
    L.append(f"| {lab} | {k6} | {us} | {bl} |")
    for g in ("k6", "blast", "users"):
        if b[g] is not None:
            prev[g] = b[g]

readme = open(sys.argv[2]).read()
b, e = "<!-- results:begin -->", "<!-- results:end -->"
block = b + "\n" + "\n".join(L) + "\n" + e
if b in readme:
    readme = readme[:readme.index(b)] + block + readme[readme.index(e) + len(e):]
else:
    readme = readme.rstrip() + "\n\n## Progress\n\n" + block + "\n"
open(sys.argv[2], "w").write(readme)
print(f"README.md updated: {len(steps)} changes from {len(rows)} runs")
