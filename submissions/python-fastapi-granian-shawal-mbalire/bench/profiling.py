"""Where CPU 0's time goes, from perf samples: used by bench/linux.py --profile.

perf samples CPU 0 on a timer (the VM has no PMU), idle included. Each run's `perf script` output is
folded into collapsed stacks (`stacks.folded`, one "frame;frame;...;leaf count" line per distinct
stack; speedscope and flamegraph.pl open it) and summarised as shares of CPU 0's *busy* samples:

- layer: the binary the sample was in (kernel, python, sqlite, granian, libc, ...);
- kernel: why the kernel ran (network receive softirq, which syscall, scheduler, other interrupts);
- thread: the Python thread, Granian's Rust I/O thread(s), interrupts that landed while CPU 0 idled;
- python: per Python function, the share of samples it was on the stack for (python mode only).

Every summary also has busy_fraction (busy / all samples), so `share x busy_fraction` is a share of
the whole core, comparable between runs at the same user count.

Stdlib only and Python 3.9 compatible, like bench/linux.py.
"""

import collections
import json
import re
from pathlib import Path

IDLE_SYMBOLS = {"cpuidle_idle_call", "default_idle_call", "cpu_do_idle", "arch_cpu_idle", "do_idle"}
# (layer, substrings of the binary's path), first match wins: the Granian .so lives under python3.x/.
LAYERS = [("kernel", ("kernel.kallsyms",)), ("granian", ("_granian",)), ("sqlite", ("libsqlite",)),
          ("uvloop", ("uvloop", "loop.cpython")), ("orjson", ("orjson",)), ("python", ("python3", "perf-")),
          ("libc", ("libc.", "ld-linux")), ("vdso", ("[vdso]",))]
SYSCALL_PREFIXES = ("__arm64_sys_", "__x64_sys_", "__se_sys_")
HEADER = re.compile(r"^(\S.*?)\s+(\d+)\s+\[(\d+)\]")


def layer_of(binary: str) -> str:
    for layer, marks in LAYERS:
        if any(mark in binary for mark in marks):
            return layer
    return "other"


def frame_label(symbol: str, binary: str) -> str:
    layer = layer_of(binary)
    tag = layer if layer != "other" else binary.rsplit("/", 1)[-1]
    return (symbol + " [" + tag + "]").replace(";", ":")


def fold_script(script: Path) -> collections.Counter:
    """`perf script` output -> {"comm-tid;root frame;...;leaf frame": samples}."""
    stacks = collections.Counter()
    head, frames = None, []

    def flush():
        if head is not None and frames:
            stacks[";".join([head] + frames[::-1])] += 1

    with open(script, errors="replace") as lines:
        for line in lines:
            if not line.strip():
                flush()
                head, frames = None, []
            elif line[0] in " \t":
                _, _, rest = line.strip().partition(" ")
                symbol, _, binary = rest.rpartition(" (")
                frames.append(frame_label(symbol.rsplit("+0x", 1)[0] or "[unknown]", binary.rstrip(")")))
            else:
                match = HEADER.match(line)
                head = (match.group(1).replace(";", ":").replace(" ", "_") + "-" + match.group(2)) if match else "?"
    flush()
    return stacks


def write_folded(stacks: collections.Counter, path: Path) -> None:
    with open(path, "w") as out:
        for stack, count in stacks.most_common():
            out.write(f"{stack} {count}\n")


def read_folded(path: Path) -> collections.Counter:
    stacks = collections.Counter()
    for line in path.read_text().splitlines():
        stack, _, count = line.rpartition(" ")
        stacks[stack] += int(count)
    return stacks


def _symbol(frame: str) -> str:
    return frame.rsplit(" [", 1)[0]


def _tag(frame: str) -> str:
    """The frame's layer. SQLite counts as sqlite even when it is linked into the python binary."""
    tag = frame.rsplit(" [", 1)[-1].rstrip("]")
    if tag == "python" and frame.startswith("sqlite3"):
        return "sqlite"
    return tag


SERVER_LAYERS = {"python", "granian", "sqlite", "uvloop", "orjson"}


def kernel_cause(frames: list) -> str:
    symbols = [_symbol(frame) for frame in frames]
    if "net_rx_action" in symbols:
        return "network receive (softirq)"
    calls = [symbol for symbol in symbols if symbol.startswith(SYSCALL_PREFIXES)]
    if calls:
        name = calls[0]
        for prefix in SYSCALL_PREFIXES:
            name = name.replace(prefix, "")
        return "syscall " + name
    if any(symbol in ("schedule", "__schedule", "finish_task_switch.isra.0") for symbol in symbols):
        return "scheduler"
    if any("softirq" in symbol or "irq" in symbol for symbol in symbols):
        return "other interrupts"
    return "other"


def summarize(stacks: collections.Counter) -> dict:
    """Shares of busy samples, by layer, kernel cause, thread and Python function."""
    total = sum(stacks.values()) or 1
    busy_stacks = [(stack.split(";"), count) for stack, count in stacks.items()
                   if _symbol(stack.rsplit(";", 1)[-1]) not in IDLE_SYMBOLS]
    busy = sum(count for _, count in busy_stacks) or 1
    # Server threads, whatever their names (granian-*, python3-*): any sample in the server's code.
    python_by_thread, server_threads = collections.Counter(), set()
    for parts, count in busy_stacks:
        tags = {_tag(frame) for frame in parts[1:]}
        if tags & SERVER_LAYERS:
            server_threads.add(parts[0])
        if "python" in tags:
            python_by_thread[parts[0]] += count
    python_thread = python_by_thread.most_common(1)[0][0] if python_by_thread else None

    layer, kernel, thread, leaf = (collections.Counter() for _ in range(4))
    python = collections.Counter()
    for parts, count in busy_stacks:
        thread_name, frames = parts[0], parts[1:]
        tag = _tag(frames[-1])
        layer[tag if tag in dict(LAYERS) else "other"] += count
        if tag == "kernel":
            kernel[kernel_cause(frames)] += count
        if thread_name == python_thread:
            thread["python thread"] += count
        elif thread_name in server_threads:
            thread["granian threads"] += count
        elif thread_name.startswith("swapper-"):
            thread["interrupts while idle"] += count
        else:
            thread["other processes"] += count
        leaf[frames[-1]] += count
        for function in {_symbol(frame) for frame in frames if frame.startswith("py::")}:
            python[function.split(":/", 1)[0]] += count

    def shares(counter, limit=None):
        return {key: round(value / busy, 4) for key, value in counter.most_common(limit)}

    return {
        "samples": total, "busy_fraction": round(busy / total, 4),
        "layer": shares(layer), "kernel": shares(kernel), "thread": shares(thread),
        "python": shares(python, 30), "top_leaf": shares(leaf, 25),
    }


def aggregate(summaries: list, results: list) -> dict:
    """Mean, min and max of every number across runs (keys missing in a run count as 0)."""
    def stat(values):
        return {"mean": round(sum(values) / len(values), 4), "min": min(values), "max": max(values)}

    out = {"runs": len(results)}
    per_request = [round(result["server_cpu_s"] / result["requests"] * 1e6, 1) for result in results]
    out["run"] = {"server_us_per_request": stat(per_request),
                  "core0_util": stat([result["core0_util"] for result in results]),
                  "p99_ms": stat([result["p99_ms"] for result in results])}
    if summaries:
        out["busy_fraction"] = stat([summary["busy_fraction"] for summary in summaries])
        for group in ("layer", "kernel", "thread", "python"):
            keys = sorted({key for summary in summaries for key in summary[group]})
            rows = {key: stat([summary[group].get(key, 0.0) for summary in summaries]) for key in keys}
            out[group] = dict(sorted(rows.items(), key=lambda item: -item[1]["mean"]))
    return out


def aggregate_hops(runs: list) -> dict:
    """{"hops": {endpoint: {hop: mean_us stat}}} across runs of the server's hops.json (may be empty)."""
    if not runs:
        return {}
    out: dict = {}
    for endpoint in runs[0]:
        for hop in runs[0][endpoint]:
            values = [run[endpoint][hop]["mean_us"] for run in runs if hop in run.get(endpoint, {})]
            out.setdefault(endpoint, {})[hop] = {
                "mean_us": round(sum(values) / len(values), 3), "min": min(values), "max": max(values),
                "count": sum(run[endpoint][hop]["count"] for run in runs if hop in run.get(endpoint, {})),
            }
    return {"hops": out}


def print_aggregate(agg: dict, limit: int = 12) -> None:
    def cell(row, percent=True):
        if percent:
            return f"{row['mean'] * 100:5.1f}%  ({row['min'] * 100:.1f}-{row['max'] * 100:.1f})"
        return f"{row['mean']:7.1f}  ({row['min']}-{row['max']})"

    print(f"\n== {agg['runs']} runs: mean (min-max)")
    for key, row in agg["run"].items():
        print(f"   {key:28} {cell(row, percent=key == 'core0_util')}")
    for endpoint, hops in agg.get("hops", {}).items():
        print(f"-- hops of {endpoint} (µs per request, elapsed; timed mode adds ~2.5-3.3 µs per read)")
        for hop, row in hops.items():
            print(f"   {hop:28} {row['mean_us']:8.2f}  ({row['min']}-{row['max']})  n={row['count']}")
    if "busy_fraction" not in agg:
        return
    print(f"   {'perf busy fraction':28} {cell(agg['busy_fraction'])}")
    for group, title in (("layer", "share of busy CPU 0 by binary"), ("kernel", "kernel time by cause"),
                         ("thread", "by thread"), ("python", "Python functions, on the stack (inclusive)")):
        if agg.get(group):
            print(f"-- {title}")
            for key, row in list(agg[group].items())[:limit]:
                print(f"   {key[:60]:60} {cell(row)}")


def save_summary(out: Path, stacks: collections.Counter) -> dict:
    summary = summarize(stacks)
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    return summary
