# Ordinary TCP with an 82% application CPU budget

This local sensitivity test moves each service-side veth receive queue onto its router lane's CPU. The application and its threads share one physical CPU with a **4,100 µs quota per 5,000 µs period**. Every server gets the same 82% ceiling. The [four-shard router harness](../router/README.md) supplies the stock scorer, synchronized ramp, disk-backed SQLite copy inside the app cgroup, memory and FD limits, private TCP ehash and network health checks.

The 82% assumption is the mean published app CPU at three five-minute holds. Willwang held 280,000 users, Kensac 310,000 and Mojo 304,000. All three reported 82%. The remaining 18 percentage points represent a modeling choice. On the test host, Linux could charge inline receive softirq time to the interrupted app cgroup, so the app and CPU-wide softirq measurements can overlap. Swift reported 92% and Rkkautsar 96% in their published holds. A fixed 82% cap can change their relative ranking. [Willwang](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/403#issuecomment-6816178650) · [Kensac](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264#issuecomment-6566431071) · [Mojo](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/257#issuecomment-6564516869) · [Swift](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/376#issuecomment-6752201104) · [Rkkautsar](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/355#issuecomment-6732991635)

The original C binary passed a clean local two-minute probe at 290,000 users with 34.6 ms p99. The organizer recorded 2,960 ms at the same load. Its local 280,000-user five-minute hold reached 66.8 ms p99 and failed the external packet-loss check. These observations limit this fixture to CPU-budget sensitivity comparisons. [Organizer report](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/403#issuecomment-6816178650)

## Run

From the challenge checkout, use the router fixture's Linux, sudo and CPU-placement prerequisites. Choose a disk-backed work directory and a fresh output tag for each trial. The standalone proof uses the default CPU map, with CPU 10 for the app and CPUs 2, 5, 8 and 14 for the four router lanes. The example uses a two-minute probe. Use `--hold 300` for a five-minute comparison.

```sh
entry="$PWD/submissions/c-uring-willwang"
official="$entry/tests/official"
work="$HOME/twelve-rx-benchmark"
mkdir -p "$work"

python3 "$official/rx-budget/fixture.py" proof --output "$work/placement"
"$official/build-client.sh" "$work"
python3 "$official/rx-budget/fixture.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
    --output "$work/runs" --users 280000 --hold 120 --tag app-280k-probe
```

Before each trial, eight ordinary TCP connections generate 32 `SO_INCOMING_CPU` observations. Startup checks all four receive CPUs, external routes and queue drops. It reads back `cpu.max`, burst, affinity, memory and swap limits from the candidate's live cgroup before starting mload. The optional `proof --natural` checks where traffic lands with the private service RPS masks disabled. On the test host, both modes observed 8/8 receives per lane on its assigned CPU and zero external drops. The scored profile always uses the four explicit lane masks. [Recorded checkout proof](placement.json)

Inspect `rx-placement/proof.json` for socket placement, `service-controls.json` for the enforced budget and `budget-telemetry.json` for per-phase CPU usage, throttled periods, throttled time and the last pre-cutoff external drop counters. The shared runner preserves the stock result and scorer inputs under the same output tag.
