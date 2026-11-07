"""Rewrites the progress section of README.md (between the results markers) from stress-results.csv:
one line per change (LABEL, in the order first measured) with its best local numbers and the change
against the previous step. Labels starting with "probe" or "smoke" are measurements, not changes: skipped.

usage: python3 -I dev/report.py <csv> <README.md>
"""
import csv, sys

rows = list(csv.DictReader(open(sys.argv[1])))
steps = []                                   # labels in order of first appearance
best = {}                                    # label -> {"k6": users, "blast": req/s}
for r in rows:
    lab = r["label"]
    if lab.lower().startswith(("probe", "smoke")):
        continue
    if lab not in best:
        steps.append(lab)
        best[lab] = {"k6": None, "blast": None}
    if r["pass"] != "1":
        continue
    gen = r.get("gen") or "k6"
    v = int(r["vus"]) if gen == "k6" else float(r["req_s"])
    if best[lab][gen] is None or v > best[lab][gen]:
        best[lab][gen] = v


def delta(cur, prev):
    if cur is None or prev is None:
        return ""
    return f" ({100 * (cur - prev) / prev:+.0f}%)"


L = ["Best local result per change. Raw runs: [stress-results.csv](stress-results.csv); how to run: above.",
     "Blast req/s (max throughput at 1 CPU) is the number to watch, with % change against the previous change.",
     "k6 users = highest passing run, not always a full ladder; this host caps k6 near 20k users.", "",
     "| change | k6 users | blast req/s |", "|---|--:|--:|"]
prev = {"k6": None, "blast": None}
for lab in steps:
    b = best[lab]
    k6 = f"{b['k6']:,}" if b["k6"] else "–"
    bl = f"{b['blast']:,.0f}{delta(b['blast'], prev['blast'])}" if b["blast"] else "–"
    L.append(f"| {lab} | {k6} | {bl} |")
    for g in ("k6", "blast"):
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
