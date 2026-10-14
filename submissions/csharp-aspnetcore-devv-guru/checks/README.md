# Verification and local benchmarks

Run commands from the repository root unless stated otherwise. These tools use
fresh database copies and leave the seed unchanged. Generated results belong in
the repository's ignored `results/` directory.

## Windows verification

Generate the seed in WSL with the upstream script first. From the submission
directory, using .NET SDK 10.0.401:

```powershell
$feedRunDirectory = Join-Path .\bin ('dev-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $feedRunDirectory | Out-Null
Copy-Item ..\..\seed\feed.db (Join-Path $feedRunDirectory 'feed.db')
$env:SQLITE_PATH = (Resolve-Path (Join-Path $feedRunDirectory 'feed.db')).Path
$env:JWT_SECRET = 'twelve-dollar-challenge'
$env:HOST = '127.0.0.1'
$env:PORT = '3000'

dotnet restore --locked-mode
dotnet publish -c Release -r win-x64 --self-contained true --no-restore -o bin/publish-win
& .\bin\publish-win\FeedApi.exe
```

The self-contained publish supplies runtime 10.0.13. Stop the manual server before
running the integration checks; the checks launch their own instances. From the
repository root:

```powershell
python submissions/csharp-aspnetcore-devv-guru/checks/integration.py --seed seed/feed.db --server submissions/csharp-aspnetcore-devv-guru/bin/publish-win/FeedApi.exe
```

Use a fresh run directory each time. Do not overwrite an open database or reuse
an unrelated WAL/SHM pair. Bash scripts need LF line endings in WSL.

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

This host's ephemeral-port range permits 28,232 source ports per IP. The runner
rejects loads exceeding that range unless enough source IPs are specified.
`--client-ips` uses k6's documented
[`--local-ips` option](https://grafana.com/docs/k6/latest/using-k6/k6-options/reference/#local-ips)
with existing loopback addresses. It changes no OS settings. At 30,000 users, a
mid-hold snapshot confirmed 15,000 connections from each of the two addresses.
An earlier single-address attempt was stopped during warm-up and excluded.

### Results: 2026-10-14

All three runs used the same published build and the conditions in the
[submission README](../README.md#local-load-results).

| Measurement | 20,000 VUs | 25,000 VUs | 30,000 VUs |
|---|---:|---:|---:|
| Measured requests | 746,431 | 933,311 | 1,119,026 |
| Central-hold CPU, one-core scale | 74.2% | 82.3% | 85.9% |
| Peak server cgroup, including charged cache | 970 MiB | 1,044 MiB | 1,130 MiB |
| Observed process RSS peak | 371 MiB | 436 MiB | 519 MiB |
| Observed k6 RSS peak | 7.14 GiB | 8.93 GiB | 10.73 GiB |
| Like HTTP p99 | 417.63 ms | 318.52 ms | 2,011.69 ms |
| Create HTTP p99 | 362.69 ms | 308.53 ms | 2,010.28 ms |
| Like failures | 0 | 0 | 6,411 (8.28%) |
| Create failures | 0 | 0 | 867 (8.47%) |
| Observed write-queue peak | 68 | 123 | 256 |

Reads had zero errors in all three runs. At 30,000, rejection counters recorded
4,465 full-queue and 2,813 admission-timeout rejections. Typical commit interval
p50s stayed near 2.1 ms, but the worst observed two-second like-commit p99 reached
1.86 seconds. This identifies write admission and commit stalls for profiling;
it does not distinguish storage effects from shared-host scheduling. No swap or
OOM kills were observed. Memory, queue and WAL samples have two-second resolution.
Cross-run tail latency varies, so these individual runs do not establish an
exact capacity ceiling. Official scoring needs the target VM and a remote client.

### Database checks

After all load tests stop, run `verify-performance.py` in WSL with a run's VU
directory, `--seed /absolute/linux/path/feed.db` and `--output /path/report.json`.
It rejects an active service, opens the databases read-only, runs `quick_check`
and foreign-key checks, and reconciles post counts with successful create
requests from warm-up and measurement. The integration suite separately verifies
the 201 response contract. This is a post-load check, not a power-failure test.

Both higher-load databases passed: 508,674 posts at 25,000 users (500,000 seed +
166 warm-up + 8,508 measured creates), and 509,516 at 30,000 (500,000 + 150 +
9,366). Local reports are `results/performance-higher-capacity-2026-10-14.json`
and `results/performance-{25000,30000}-integrity-2026-10-14.json`. These generated
files are not included in the submission.

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
Run metadata includes the actual published DLL hash, source hashes and limits.

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

## Optimization comparisons

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
