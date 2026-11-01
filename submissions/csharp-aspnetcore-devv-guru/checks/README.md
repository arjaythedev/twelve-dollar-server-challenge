# Verification and local benchmarks

Run commands from the repository root unless stated otherwise. These tools use
fresh database copies and leave the seed unchanged. Generated results belong in
the repository's ignored `results/` directory.

## Native build

The benchmark target is Ubuntu 24.04 x64. Build and test inside Linux/WSL using
the scripts in the submission README. `install.sh` includes clang and zlib
development headers for Native AOT. The .NET SDK, ILCompiler and NuGet versions
are pinned; restore uses the updated lock file.

The final native publish completed with warnings treated as errors under a
one-core CPU quota, 2 GiB memory limit and no swap. Its observed build cgroup
peak was 1,102,098,432 bytes. The executable was about 12 MB, plus the SQLite
native library; separate debug symbols remain available in the publish folder.
The measured executable SHA-256 is
`51517c3bd97d35d23bb235f08aed9e6d381182f20f249e4948ca147d36c6ac71`;
the bundled `libe_sqlite3.so` SHA-256 is
`4dc77b4c5c49be108470ada98911a5db7af9be23e923fdc2d00e2b93f55d70ac`.
Source, project and lock-file hashes match the measured build's manifest.

The Linux executable passed the 42 official and 52 exclusive-access checks,
including a complete escaped/Unicode feed over 60 KB with exact Content-Length.
The preserved PR #195 baseline also passed its original 46 shared-access checks
with `integration.py --database-access shared`. The exclusive profile checks
live API freshness and rejected external access, and stops the server for SQL
inspection, external edits and backup. Acknowledged creates are checked after
an immediate process kill, before graceful shutdown can affect the result.

Use fresh database copies. Do not overwrite an open database or reuse an
unrelated WAL/SHM pair. Bash scripts need LF line endings in WSL. The Windows
performance runner below orchestrates Linux builds; it does not run a Windows
Native AOT executable.

## Load testing

`local-performance.py` runs Linux k6 and the Linux server from Windows. It needs
WSL Ubuntu 24.04 with systemd, a Linux publish from `build.sh`, and Linux k6
2.3.0 extracted to the submission's `.tools/k6/k6-v2.3.0-linux-amd64/k6`.
Use the [official k6 release](https://github.com/grafana/k6/releases/tag/v2.3.0).
The `k6-v2.3.0-linux-amd64.tar.gz` SHA-256 used here was
`39c3117b6af817592dcd0ce4242105c0a7af10948c2a425306f0be8f7a8a8ab1`.

Keep the seed and test databases on the native Linux filesystem. From WSL at
the repository root, copy the seed once before testing:

```bash
mkdir -p "$HOME/.cache/twelve-dollar-server-challenge/seed"
cp seed/feed.db "$HOME/.cache/twelve-dollar-server-challenge/seed/feed.db"
```

From Windows PowerShell:

```powershell
python submissions/csharp-aspnetcore-devv-guru/checks/local-performance.py --vus 25000 --hold 5m --ramp 60s --warmup 2m
python submissions/csharp-aspnetcore-devv-guru/checks/local-performance.py --vus 30000 --hold 5m --ramp 60s --warmup 2m --client-ips 127.0.0.1,127.0.0.2
python submissions/csharp-aspnetcore-devv-guru/checks/local-performance.py --vus 40000 --hold 5m --ramp 60s --warmup 2m --client-ips 127.0.0.1,127.0.0.2 --client-memory-limit 12GiB --collect-counters
python submissions/csharp-aspnetcore-devv-guru/checks/summarize-performance.py results/performance-<run-id> --output results/performance-analysis.json
```

Do not run multiple harnesses concurrently. Each level uses a fresh database and
temporary systemd unit. The runner verifies CPU quota, memory/swap limits and
the 65,535 file-descriptor limit. It copies the database inside the unit so its
page cache is charged to that cgroup. k6 runs outside the server cgroup.

The upstream `bench/load.js` workload is imported unchanged. Extra metric filters
retain endpoint summaries without changing scoring thresholds. The original
20,000-token file is reused cyclically at higher VU counts; virtual users are
simulated sessions with think times, not distinct accounts or requests/sec.

The k6 child uses `GOGC=50` and `GOMEMLIMIT=10GiB` by default to keep larger
tests within this WSL host's memory. These affect the client only; the memory
limit is a Go runtime soft limit, not an application/server setting. Override
them with `--client-gogc` and `--client-memory-limit`. Both values are recorded
in run metadata. Reject results affected by client swapping, OOM or saturation.
Published-code hashing uses the native executable for AOT and the application
DLL for JIT builds.

This host's ephemeral-port range permits 28,232 source ports per IP. The runner
rejects loads exceeding that range unless enough source IPs are specified.
`--client-ips` uses k6's documented
[`--local-ips` option](https://grafana.com/docs/k6/latest/using-k6/k6-options/reference/#local-ips)
with existing loopback addresses. It changes no OS settings. At 30,000 users, a
mid-hold snapshot confirmed 15,000 connections from each of the two addresses.
An earlier single-address attempt was stopped during warm-up and excluded.

### Final read-path build: confirmation hold

The final Native AOT executable passed the unchanged challenge thresholds at
45,000 VUs, with a fresh database, two-minute 1,000-VU warm-up, 60-second ramp,
five-minute hold, 30-second ramp-down and ten-second graceful stop. Runtime/DB
counters were enabled. Client GOGC=100 / GOMEMLIMIT=12GiB and two loopback source
IPs match the earlier higher-load setup.

| Measurement | Final build |
|---|---:|
| Requests | 1,678,637 |
| Requests/sec including ramps | 3,957.44 |
| HTTP p95 / p99 | 43.96 / 106.93 ms |
| HTTP errors | 0 |
| Central-hold server CPU, one-core scale | 63.87% |
| Server cgroup / observed process RSS peak | 1,031 / 699 MiB |
| Client observed RSS peak | 11,866 MiB |

No observed client/server swapping or OOM kills. The stopped database passed
quick_check and foreign-key checks; its 515,691 posts reconcile to
500,000 seed plus 164 warm-up and
15,527 measured acknowledged creates. This is
one local hold and does not establish the target VM's score or an exact ceiling.
The earlier exclusive build at 45k averaged 81.46% CPU and 700.80 ms p99;
overall throughput was similar at 3,986 req/s. This is one full comparison,
with the changes combined and no isolation of their individual contributions.

Both higher-load attempts had zero HTTP errors but failed p99:

| Users | Requests/sec | p95 / p99 | Thresholds |
|---|---:|---:|---|
| 47,500 | 4,122.98 | 425.04 / 1,076.27 ms | Fail (p99) |
| 50,000 | 4,350.82 | 334.16 / 1,042.58 ms | Fail (p99) |

The lower-load retry had worse p99, demonstrating local variability. These
runs do not establish a capacity increase over the earlier 45k build. Both
stopped databases passed consistency, foreign-key and acknowledged-create
reconciliation checks. Raw failed runs are
`results/performance-20261027T225949Z-2244cf` (50k) and
`results/performance-20261027T231033Z-fba98e` (47.5k).
Raw final run: `results/performance-20261027T232131Z-4ef71b`.
Final reports are `results/native-read-final-analysis.json` and
`results/native-read-final-integrity.json`. Generated reports remain ignored.

### Profiling and read-path changes: 2026-10-27

GNU gprofng sampled the native executable with its debug symbols during a
30-second, 64-VU diagnostic workload under the same one-core/2 GiB/no-swap
server limits. The original profile attributed 12.12% inclusive sampled CPU to
`sqlite3RunParser`, reached through `sqlite3Reprepare` during `sqlite3_step`.
With QPSG and immediate write-permit acquisition, parser samples fell to 0.31%
and the reprepare frame had no samples. These overlapping inclusive frames must
not be added, and no samples is not proof of zero calls.

A separate native statement-counter diagnostic using the actual bundled
SQLite 3.54.0 changed QPSG only: 26 ID lookups produced five automatic reprepares
with it off and zero with it on. Response BLOB bytes, missing-ID behavior and
the tested EXPLAIN QUERY PLAN rows matched. `ENABLE_STAT4` was false in this
library; the evidence establishes suppressed bound-value replanning, without
claiming a STAT4 cause. QPSG retains query plans, not results.

Two balanced short pairs per change used fresh servers/databases, 64 constant
VUs without think time, ten-second warm-up and 30-second measure. All measured
runs had zero HTTP errors. These are diagnostic rates, not leaderboard scores.

| Change | Baseline mean req/s | Candidate mean req/s | Paired changes | Decision |
|---|---:|---:|---|---|
| Remove write limiter entirely | 8,235 | 7,055 | -16.60%, -12.11% | Rejected |
| Immediate permit acquisition + QPSG | 8,376 | 11,023 | +29.18%, +34.41% | Retained |
| Raw UTF-8 reads after the preceding changes | 11,147 | 11,552 | +4.09%, +3.12% | Retained |

The plan-stability candidate combines two changes; the throughput comparison
does not isolate their contributions. The raw-reader comparison uses that
candidate as its baseline. It removes intermediate managed response arrays and
ADO.NET reader machinery. Kestrel can copy pre-header memory into its output
pipe, so this is not described as zero-copy. SQLite memory is copied while valid;
the statement reaches DONE and resets before the destination advances, and the
network flush is outside the database lock.

A separate sequential allocation pair attached the pinned System.Runtime
collector before traffic (1s sampling), then stopped each server and waited for
export. Summed allocation increments divided by warm-up+measured request counts
were 3,636.06 bytes/request for plan-only versus 1,720.73 for direct reads
(-52.68%). Both had zero HTTP errors/failed checks and identical SQLite library
hashes. This approximate allocation comparison is not a resident-memory claim;
sampling boundaries and background/startup work can affect the totals. The
report is `results/raw-allocation-comparison.json`.

Local sources/reports are ignored: `results/qpsg-diagnostic.py` and `.json`,
`results/next-optimization-probes.json`, and
`results/profile-{current,plan}-functions.txt`. The profile is for locating work;
the unprofiled pairs are used for throughput comparisons.

### Single-connection comparison: 2026-10-27

The candidate uses one prepared connection under a repository lock with SQLite
exclusive ownership. The preserved baseline is the initial PR #195 executable,
commit `eadcc59472f2be3783676d45741b50b64cd99b88`.

Both runs imported the unchanged challenge workload: fresh server/database,
two-minute 1,000-VU warm-up, 60-second ramp, five-minute hold, 30-second ramp-down
and ten-second graceful stop. Both used the same counters, two loopback source
IPs and client GOGC=100 / GOMEMLIMIT=12GiB. The candidate ran first, then baseline.
Neither had observed app/client swapping or OOM kills.

| Measurement | Preserved pooled baseline | Single connection / EXCLUSIVE |
|---|---:|---:|
| Users | 45,000 | 45,000 |
| Challenge thresholds | Fail (p99) | Pass |
| Requests | 1,666,427 | 1,670,166 |
| Requests/sec including ramps | 3,926 | 3,986 |
| HTTP p95 / p99, ms | 464.10 / 1,257.75 | 357.68 / 700.80 |
| HTTP errors | 0 | 0 |
| Central-hold CPU, one-core scale | 82.9% | 81.5% |
| Peak server cgroup, including cache | 1,429 MiB | 1,440 MiB |
| Observed process RSS peak | 723 MiB | 728 MiB |
| Client mean CPU, cores | 4.02 | 4.28 |
| Observed client RSS peak | 11,629 MiB | 11,758 MiB |

This single pair supports keeping the observed tail-latency improvement. It
does not establish a portable capacity ceiling or a large throughput, CPU or
memory gain. Three short saturation probes per variant were inconclusive:
64 constant VUs without think time, ten-second warm-up and 30-second measure,
fresh server/database and the same server limits. Mean RPS was 8,416 versus
8,541 (+1.48%); median RPS was 8,415 versus 8,423 (+0.09%). Paired differences
were +4.46%, -4.49% and +4.90%, with zero HTTP errors in all six measurements.

The full runs are `results/performance-20261027T215559Z-f8094b` (candidate) and
`results/performance-20261027T220715Z-d50c0e` (baseline). Reports are
`results/single-connection-full-comparison.json` and
`results/single-connection-probes.json`. These generated files remain ignored.

The baseline binary hash matches both the preserved executable and the earlier
40k run. Its metadata's source hashes describe the candidate workspace because
`--publish` selects an external executable; they are not the baseline's source
provenance. The baseline's source reference is the commit above.

Both stopped databases passed `quick_check`, foreign-key checks and successful
create reconciliation. The candidate had 515,315 posts (500,000 + 126 warm-up +
15,189 measured); baseline had 515,599 (500,000 + 167 + 15,432). Reports are
`results/single-{candidate,baseline}-45000-integrity.json`.

### Earlier comparisons: 2026-10-27

The merged PR #95 build was republished from its unchanged sources. Each run used
a fresh database/server, two-minute 1,000-VU warm-up, 60-second ramp, five-minute
hold, 30-second ramp-down and ten-second graceful stop. All three collected the
same runtime/database counters. Rates and latency include ramp/drain.

| Measurement | FULL baseline, 30k | Pooled/socket-tuned, 40k | Before socket tuning, 50k |
|---|---:|---:|---:|
| Challenge thresholds | Fail | Pass | Fail |
| Requests | 1,089,167 | 1,489,414 | 1,767,437 |
| Requests/sec | 2,598 | 3,523 | 4,199 |
| HTTP p95 / p99, ms | 1,846.67 / 3,049.09 | 225.95 / 455.24 | 1,383.32 / 2,370.65 |
| HTTP errors | 8,902 (0.817%) | 0 | 0 |
| Central-hold CPU, one-core scale | 83.0% | 81.1% | 97.6% |
| Peak server cgroup, including cache | 1,154 MiB | 1,350 MiB | 1,311 MiB |
| Observed process RSS peak | 512 MiB | 661 MiB | 639 MiB |
| Observed k6 RSS peak | 8,306 MiB | 11,135 MiB | 11,821 MiB |

The baseline errors were 7,852 likes and 1,050 creates; reads had zero errors.
The 50k run completed with zero errors but failed both latency thresholds. These
different loads do not measure each optimization's contribution or establish an
exact capacity ceiling. The official target VM and a remote load generator are
needed for a leaderboard score.

The baseline used client GOGC=50 / GOMEMLIMIT=10GiB; 40k used 50 / 12GiB and 50k
used 100 / 12GiB. Client CPU averaged 2.53, 2.81 and 7.90 cores respectively on
this 16-logical-CPU WSL host. No swap or OOM kills were observed. An earlier 50k
attempt with a 10GiB client soft limit saturated the load generator and was
aborted; it is excluded. Memory and CPU observations have two-second resolution.

Raw runs are in ignored directories `results/performance-20261027T201539Z-62b9f3`,
`results/performance-20261027T212427Z-27f3de` and
`results/performance-20261027T205725Z-64b2da`. The combined local report is
`results/followup-comparison-2026-10-27.json`.

Before socket tuning, the same AOT/NORMAL/SQLite JSON build also passed 40k at
3,555 requests/sec and 110.76 / 457.68 ms p95/p99, with zero errors. That raw run
is `results/performance-20261027T211013Z-3b611f`. A separate 64-VU saturation
probe increased from about 7,704 to 8,335 requests/sec with the socket settings;
that short, no-think-time probe is diagnostic and is not a challenge score.

At 40k, the socket-tuned pooled run used 81.1% of one core versus 88.9% before socket tuning,
with 661 versus 523 MiB observed process RSS. Its p95 increased, while p99
was similar. This single pair supports a CPU/memory tradeoff, not an exact speedup.

### Database checks

After all load tests stop, run `verify-performance.py` in WSL with a run's VU
directory, `--seed /absolute/linux/path/feed.db` and `--output /path/report.json`.
It rejects an active service, opens the databases read-only, runs `quick_check`
and foreign-key checks, and reconciles post counts with successful create
requests from warm-up and measurement. The integration suite separately verifies
the 201 response contract. This is a post-load check, not a power-failure test.

All four earlier pooled/socket comparison databases passed these checks. That 40k
database had 513,878 posts: 500,000 seed rows plus 170 warm-up and 13,708 measured
successful creates.
The final report is `results/followup-final-40000-integrity.json`.

## Diagnostics

Install the optional pinned collector inside WSL:

```bash
/opt/twelve-dollar-dotnet/10.0.401/dotnet tool install dotnet-counters \
  --version 10.0.750501 \
  --tool-path "$HOME/.cache/twelve-dollar-server-challenge/tools/diagnostics"
```

Add `--collect-counters` to a performance run to collect `System.Runtime` and
`DevvGuru.FeedApi`. Collection adds application work, so use the same setting for
comparisons. CSV timestamps are UTC; the tool exports the file when collection
ends. `--publish /windows/path/to/linux-publish` selects a comparison build.
Run metadata includes the actual published executable/DLL hash, source hashes and limits.

| Instruments | Meaning |
|---|---|
| `feed.db.duration`, `feed.db.commit`, `feed.db.write_wait` | Millisecond histograms; operation tags distinguish reads and writes |
| `feed.db.write_queued`, `feed.db.write_active` | Current admission queue and active writer |
| `feed.db.write_rejected` | Rejection count, tagged by reason |
| `feed.runtime.worker_threads`, `feed.runtime.pending_work` | Absolute thread-pool snapshots |

The analyzer uses central-hold CPU and allocation samples. Histogram rows are
two-second interval quantiles; averaging them does not yield a whole-run p99.
Runtime UpDownCounter rates are not absolute thread/queue counts, so the analyzer
uses the explicit gauges above.

With the single-connection implementation, write-queue gauges measure admission
to the write permit. They do not include time waiting for the connection lock;
operation durations include that wait. All SQLite work, recovery and disposal
use that same lock.

## Original optimization comparisons

At 20,000 users, one before/after pair for prepared-session reuse reduced managed
allocation from 16.55 to 13.18 MiB/s and HTTP p99 from 348.47 to 100.10 ms. Write
failures fell from 130 to zero. Typical commits remained near 2.1 ms; both builds
used FULL synchronization. Baseline database verification briefly overlapped the
updated run outside its cgroup, so shared-host effects cannot be fully excluded.
The local report is `results/performance-prepared-comparison-2026-10-14.json`.

`startup-performance.py` alternates publish variants over fresh processes, using
a fresh database per launch and a warm OS file cache. It records startup/first
request times, RSS, publish size and application hashes. The startup driver and
server share the chosen cgroup. An example from Linux:

```bash
sudo systemd-run --quiet --wait --pipe --collect \
  --property="User=$(id -un)" --property=CPUQuota=100% \
  --property=MemoryMax=2G --property=MemorySwapMax=0 \
  python3 "$PWD/submissions/csharp-aspnetcore-devv-guru/checks/startup-performance.py" \
  --seed "$HOME/.cache/twelve-dollar-server-challenge/seed/feed.db" \
  --variant before=/absolute/path/to/before-publish \
  --variant after=/absolute/path/to/after-publish \
  --runs 7 --output "$PWD/results/startup-comparison.json"
```

Seven alternating samples of the initial versus refactored/generated-handler/
ReadyToRun build measured median readiness at 375.3 versus 301.2 ms, first feed
at 8.78 versus 5.77 ms, and first create at 63.47 versus 36.87 ms. Publish size
grew by 1.16 MiB. This measures combined changes, not each flag's contribution.
The local report is `results/startup-final-2026-10-14.json`. Use separate clean
builds when changing generator properties: incremental compilation can otherwise
reuse the same generated handlers across variants.
