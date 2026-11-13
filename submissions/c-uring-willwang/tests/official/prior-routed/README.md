# Historical routed comparison

This local Linux fixture reproduces the network and scoring program recovered from the earlier leaderboard investigation. One router connects the server and a shared generator network namespace using two veth pairs. Four stock mload processes use distinct offsets and address sets. Each private link has MTU 1500 and GRO enabled. Both router egresses have 30 µs netem delay. The historical calibration measured 0.249 ms median idle RTT. The server and Linux receive steering use CPU 10. The router uses CPUs 9 and 14, the four generators use 0–2, 3–5, 6–8 and 11–13, and the manager uses CPU 15. The full scored interval includes the linear ramp and the requested hold.

## Build and run

Run as the ordinary benchmark user. The host needs Linux networking namespaces, veth GRO/RPS, iproute2, ethtool, a working user systemd manager with cgroup v2 memory control, and permission for private namespace setup using `sudo -n`. The reproduced local server-process budgets are 1,750 MiB, zero swap and 65,535 open files. Every app and scorer starts without capabilities. The stock binary is checked against its archived SHA-256, `48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6`. A 300k trial needs enough free host memory and file descriptors for the four load generators. The collector records actual scope memory/CPU and packet delivery.

`build.py` accepts a challenge checkout and an independent writable work directory. It restores the measured driver from the checkout’s public PR403 test source and a unified diff. It compiles the original placement program from that checkout, then checks the restored sources and compiled ELF against all seven original SHA-256 values. The matching environment used Ubuntu 24.04 GCC 13.3.0-6ubuntu2~24.04.1 on x86-64. Compilers producing another placement binary will report its digest. The build writes its compiled output to the selected work directory.

```sh
python3 build.py --checkout "$challenge" --work "$fixtures"
mkdir -p "$locks"
python3 replay.py --work "$fixtures" --output "$results" \
  --component-lock "$locks/cpu9-bench.lock" --serial-lock "$locks/active.lock" \
  --cwd "$challenge" --server "$server" --mload "$mload" \
  --seed "$challenge/seed/feed.db" --tokens "$challenge/seed/tokens.json" \
  --users 128 --hold 45 --tag priorroute-smoke --dry-run
```

Remove `--dry-run` when the benchmark has its scheduled slot. Give every run a fresh `priorroute-` tag. Use the same two lock files for every application and related load test. `--mload-build-info FILE` supplies a saved `go version -m` record on hosts lacking the Go executable. The server must support the stock `HOST`, `PORT`, `SQLITE_PATH` and `JWT_SECRET` environment. `--cwd` establishes the harness working directory. The archived launcher creates each private SQLite copy and runs the server in its result directory. Use 300 hold seconds to compare a five-minute result at a fixed user target. The runner computes ramp seconds as `max(60, ceil(3 × users / 10000))`. The historical investigation inferred this schedule from public results. The organizer has yet to publish its per-shard ramp arguments.

To rebuild the historical inputs from a fresh challenge seed, use a Python interpreter linked to Ubuntu’s SQLite 3.45.1. Choose a new directory on a volume with space for the 249 MB database and 5 MB token file. The helper opens the original database read-only, builds the historical page layout in memory, and checks both output sizes and SHA-256 digests before publishing the directory.

```sh
/usr/bin/python3 -B restore_inputs.py --seed "$challenge/seed/feed.db" \
  --tokens "$challenge/seed/tokens.json" --output "$fresh_path" \
  --component-lock "$locks/cpu9-bench.lock" --serial-lock "$locks/active.lock"
```

For an exact earlier input comparison, supply `--input-profile historical-exact` with a new `priorroute-exact-` tag and the earlier database and token files. The launch verifies their SHA-256 values, respectively `102193bcdb4ced52abb98f6bdfb19e06284273af30b74cccd009b257f09569cc` and `7c4eb45a67156ab3827d6fadab40c884fcab77d4bc4051bffe9e537a16fffa50`. Under `common`, every implementation in a matched group must still use the same two current inputs. The original `source/run-native` is retained for SHA verification. Its legacy loopback-only absolute branch is unused by the routed launcher.

## Read the result

```sh
python3 collect.py "$results/priorroute-smoke"
python3 collect.py --matched-cohort "$results/priorroute-server-a" "$results/priorroute-server-b"
```

The first command verifies the four stock shard histograms, the full stock merge, input and environment identities, scope and private-interface records, and exact lock coverage. The cohort command additionally requires a common target, ramp/hold, input hashes, topology and signed TCP hash-table geometry, with one distinct executable hash per supplied trial.

`raw/score.json` is the unmodified stock result. `raw/plan.json`, `raw/runtime.json`, `raw/shard*.json` and `raw/samples.jsonl` preserve the measurement. `placements.json` records signed namespace table readbacks and four private TCP proof connections, each sampled through 24 exchanges. The manager and each child namespace report their local socket-table size. The organizer’s setting is unpublished. `charged-descendants.jsonl` records actual members of the transient service scope and their per-thread affinity. `scored-wire-cpu.json` records interface deltas and CPU ownership from score start to 250 ms before cutoff, separately for the full ramp plus hold and for the hold alone. The native score and duration include the final 250 ms.

`stock_pass` is the direct stock verdict. `historical_eligible` applies the archived 99% connection-floor rule at available aligned five-second samples from ten seconds after ramp. `full_presence` requires all assigned users on each of the four shards at every scheduled five-second tick after ramp and before cutoff. The distinct-cohort check combines that presence with the exact assigned number of successful TCP dials on each shard. `qualified` also requires the scored service to survive, observed CPU/memory placement across the interval and clean, complete infrastructure records.

The observer saves the beginning and ending times of read-only private qdisc measurements close to the scheduled cutoff. A positive counter observed before cutoff is a scored drop. An increase after a verified clean read beginning after cutoff belongs to teardown. A counter that first increases across the cutoff leaves strict qualification unresolved. A monotonic counter still at zero after stock merge proves the entire scored interval clean. Missing readings never imply zero. Service exit and OOM use the same time-bounded principle, including the server PID, freshness of each cgroup sample and its cumulative OOM counters. A post-cutoff exit can qualify when fresh evidence established a healthy process through the cutoff. An OOM first observed in an interval spanning cutoff leaves its timing unresolved. The collector recomputes these conditions from raw evidence. Use `python3 test_cutoff.py` for the offline regressions. Client counts are per-shard snapshots. The server’s instantaneous TCP established counter includes CLOSE_WAIT, and counts can differ around ramp and teardown.

## Directional receive steering

The optional `--profile priorrps` assigns the router’s service-facing receive queue to CPU 9 and its generator-facing receive queue to CPU 14. The historical setting lets both queues use CPUs 9 and 14. The same twelve generator CPUs, service CPU 10, management CPU 15, stock mload, links and delay stay in use. A Linux 6.8 private network namespace supplies the receive mask when each veth is created. The two additional private sysctl writes immediately precede the corresponding link creations. A read-only sysfs mount scoped to each private namespace records the actual queue masks and MTU before the scorer starts. Four real TCP probe connections also record the receiving CPU at both endpoints.

| Original C at 280,000 | Stock p99 | Requests/s | Router receive drops | Full five-second hold windows |
|---|---:|---:|---:|---:|
| Historical CPU assignment | 40.62 ms | 26,758 | 547,675 | 59/59 |
| One CPU per direction | 36.41 ms | 26,766 | 1,043,195 | 59/59 |

Both completed 280,000 distinct dials with zero application errors. Both failed the local strict qualification for packet loss. Under directional steering, the generator-facing interface dropped 1,043,002 packets and CPU 14's softnet backlog counter increased by exactly that amount. The service-facing interface and CPU 9 each recorded 193. The generator and server interfaces and CPU 10 recorded zero. This control increased router drops per 1,000 completions from 53.30 to 101.50. [Measurements, source digests and exact cutoff bounds](directional-results.json)

Use the build, inputs and two shared lock files from the earlier command. Start with `--profile priorrps --users 128 --hold 45 --tag priorrps-stock-smoke-128 --dry-run` and use those options in its scheduled slot. The measured smoke reached all participants and passed both checks. For the separate high-load control use `--profile priorrps --users 280000 --hold 300 --tag priorrps-stock-280-h1`. The ordinary collector verifies the launch-time steering source, actual local queue masks, stock data and timed qualification for both trial directories. The following comparison reproduces the stored evidence from caller-selected archive paths.

```sh
python3 directional_evidence.py --smoke "$results/priorrps-stock-smoke-128" \
  --original "$results/priorroute-stock-280-h1" \
  --directional "$results/priorrps-stock-280-h1"
```
