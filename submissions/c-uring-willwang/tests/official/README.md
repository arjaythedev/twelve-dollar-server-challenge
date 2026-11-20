# Reproduce the local run with the organizer’s Go scorer

`mload/` contains the unmodified Go scorer, module file and MIT license from `arjaythedev/million-users-challenge` at `ae4e5a74b399d41851b2f188c91122aaa6d9b7a0`. Its Go source is identical to the `bade685` version used in published comparisons for the $12 challenge. It follows the four-route browsing sequence, extracts point-request IDs from real feeds, chooses authenticated writes, and includes the ramp in the reported p95, p99, error rate and throughput. The $12 settings are a think scale of 1 and a 60-second request timeout, with gzip disabled. [Organizer source](https://github.com/arjaythedev/million-users-challenge/blob/ae4e5a74b399d41851b2f188c91122aaa6d9b7a0/bench/mload/main.go) · [Published comparison](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/3effe1d05ff3cdde7eb5b4695d1b8e6c2fac1f1c/submissions/c-iouring-unorig-username#measurements-on-x86) · [Leaderboard methodology](../../../../LEADERBOARD.md)

## Build and run

Use Linux, Go 1.27.1 or newer, Python 3.11 or newer, Bubblewrap, and a user systemd manager with cgroup v2 memory accounting. Go 1.27.3 on linux/amd64 reproduces the recorded scorer binary SHA-256 `48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6`. `build-client.sh` verifies both source files before building. It builds offline with the Go standard library and saves the compiler settings and executable hash. `GO_BIN=/path/to/go` selects a particular compiler.

From the challenge repository root, build the server with its normal scripts and choose a working directory on a disk-backed filesystem. A new tag is required for each run. The default CPU layout uses CPU 10 for the server and CPUs 0–8 and 11–13 for the four generator processes. Select distinct available CPUs on your machine.

```sh
repo=$PWD
scripts=$repo/submissions/c-uring-willwang/tests/official
server=$repo/submissions/c-uring-willwang/bin/server
work=$(mktemp -d)
"$scripts/build-client.sh" "$work"

"$scripts/run.sh" "$work" "$server" --users 350000 --preflight
"$scripts/run.sh" "$work" "$server" candidate-350k --users 350000 --ramp 60 --hold 300 --verify-close
```

`--preflight` verifies the scorer and server, hashes the seed files, checks memory and descriptor headroom, and prints the CPU and target-address plan without opening any connections. A small full-path check on a three-CPU machine can use the following settings. The short check exercises the runner, while the scored local condition uses the 60-second ramp and 300-second hold above.

```sh
"$scripts/run.sh" "$work" "$server" smoke --users 8 --ramp 2 --hold 8 \
    --server-cpu 0 --client-groups '1;2'
```

The wrapper starts a fresh private loopback network namespace. It copies `seed/feed.db` inside the server's cgroup, pins the server and its children to a single CPU, applies a 65,535 hard file-descriptor limit and disables swap. The default `--memory-mib 1750` reproduces the local runs with this scorer. Cgroup peak includes charged socket memory and the filesystem cache. The OS and generators have additional memory on this host. The organizer runs the entire OS and server in a 2 GB VM. To run from outside a challenge checkout, pass `--source /path/to/feed.db --tokens /path/to/tokens.json`.

Each shard receives a disjoint user range and a common wall-clock start. The runner distributes connections across private `127/8` destination aliases because the stock client has no source-address option. With a 32,768–60,999 ephemeral-port range, its default `--port-occupancy .15` uses 21 addresses per 87,500-user shard at 350,000 users, and 24 per 100,000-user shard at 400,000. Greater occupancy can make the load generators spend the ramp scanning for source ports. `plan.json` records the actual aliases. Use the same settings and run each server sequentially for a comparison. On kernels exposing a negative `/proc/sys/net/ipv4/tcp_ehash_entries`, even private network namespaces can share the established-socket hash table.

## Read the result

The scorer writes original per-shard output to `shard*.json` and merges the histograms into `score.json`, all under `$work/runs/<tag>`. The merged `pass` applies the organizer’s thresholds to all requests completed during the ramp and hold. Each shard also writes its five-second progress windows, connection latency, total dial attempts, error categories, endpoint-specific latency histograms, and its observed open socket count. A GET on a stale connection can trigger a fresh dial and one retry. The generator snapshot precedes shutdown.

`summary.json` adds rate, p95, p99, error count, server cgroup peak memory, CPU per request, and sampled established connections. `samples.jsonl` records five-second CPU and memory measurements, client CPU and RSS, and private TCP counters, including handshakes, retransmissions, timeouts, delayed ACKs, and keep-alive probes. CPU time charged to the server's cgroup and whole-host per-core user, system and softirq ticks have separate fields. The per-core values include all work scheduled on each selected CPU. `plan.json` records seed, token, scorer and server hashes, compiler, kernel, resource limits, and network configuration.

The organizer uses separate 16-core load generators with a 0.25 ms network RTT. Local loopback on Linux 6.8 had an IP path MTU of 65,535. A 24-request placement check saw every response arrive on the server CPU and every request arrive on the client CPU, with 28–29 µs smoothed TCP RTT. An internal Docker bridge measured MTU 1,500 and the same CPU placement. These local results allow paired comparisons on the same machine. The leaderboard measures the server on the organizer’s VPS. [Official environment](../../../../LEADERBOARD.md)

## Observe memory during the final disconnect

The optional observer reads cgroup v2 memory counters every 100 ms from five seconds before the stock cutoff through scope removal or 30 seconds afterward. It waits up to 60 seconds for the new run’s `plan.json` and `runtime.json` and checks the scope’s configured memory limit. Run it on a spare CPU outside the server and generator groups. The report goes next to the run directory so the runner can create its fresh tag.

```sh
tag=candidate-338750-tail
observer_cpu=15
taskset -c "$observer_cpu" python3 "$scripts/scope-tail.py" "$work/runs/$tag" \
    > "$work/$tag-scope-tail.json" &
observer=$!
"$scripts/run.sh" "$work" "$server" "$tag" --users 338750 --ramp 60 --hold 300
wait "$observer"
```

Each row records seconds relative to the scheduled end, `memory.current`, the cumulative `memory.peak`, all `memory.events` counters, and the `sock`, `kernel`, `slab`, `file` and `anon` categories in bytes. The two `largest_sampled_*` fields retain the rows with the largest peak read on each side of the cutoff. An increment in `events.oom_kill` records a kernel OOM kill. An increment in `events.max` counts charges that hit the configured limit. The kernel exposes the fields as separate reads, so a final row can be marked `partial` if the scope vanishes mid-sample. A missing phase has a `null` summary. If the scope disappears before any sample, the observer writes the empty report and exits nonzero. A new memory spike or OOM between the last read and removal can go unobserved. The stock score and the runner’s five-second samples are independent of this helper.

Add `--verify-close` to `run.sh` to hold the original server open after writing the stock score. Once all four generators exit, the runner gives the private namespace up to 120 seconds to reach zero `CurrEstab`, which includes `CLOSE_WAIT`. It then asks the same process for `/health` and records its response, pidfd liveness, cgroup peak and OOM counters in `close.json`. A failed shutdown check makes the runner exit unsuccessfully and preserves the original score and closure report.

## Compare receive placement

An alternative local runner creates direct, private Ethernet links in disposable containers and steers each end’s TCP receive processing to that end’s own CPUs. Its placement test verifies the result using real TCP traffic. The guide includes a 16-user smoke, the full 350,000-user command and the separate server and client CPU accounting. [Private Ethernet reproduction](veth/README.md) · [Measured differences between topologies](network-topologies.json)
