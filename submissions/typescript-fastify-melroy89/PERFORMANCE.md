# Local performance qualification

Measured on an AMD Ryzen Threadripper 7960X Linux host with 24 physical cores,
48 hardware threads and approximately 125 GiB usable host memory. The Ubuntu 24.04 server container
has a one-CPU quota, 2 GiB memory limit, no swap and `nofile=65535`.
No CPU affinity or host kernel settings were changed. The generator runs
outside the server quota on the same host, not on a separate benchmark machine.
The host OS is outside the container's memory budget. These are local diagnostic
results, not the organiser's DigitalOcean score or a demonstrated maximum.

## Serialization comparison

Fastify 5.12.7, Node 24.15.0 and source-built better-sqlite3 13.0.3. Both variants
use identical connection handling, pragmas and authentication. Autocannon 8.0.0,
100 keep-alive connections, two 10-second trials per endpoint and variant, with
the order reversed in the second round. Each server starts from a fresh seed.
Values below are medians of the two trials. These saturation tests have no
user think time and are separate from the official workload.

| Read serialization | Feed requests/s | Single-post requests/s |
| --- | ---: | ---: |
| Fastify compiled response schema | 19,644 | 44,218 |
| SQLite JSON without reparsing | 23,415 | 41,857 |

The implementation retains SQLite JSON for feeds (**19.2% higher throughput**)
and Fastify's compiled serializer for individual posts. Each post is escaped
by SQLite's JSON functions; only the fixed array envelope and comma separators
are concatenated. No database results are remembered between requests.

Earlier exploratory tests found that wrapping already-generated SQLite JSON in
additional `json()` calls lost the benefit, and gzip compression roughly halved
feed throughput. Those exploratory tests used the package's bundled native
binding. They informed the candidates; the table above was rerun with the
required source-built binding and host networking.

## Capacity testing

The unmodified official `bench/load.js` drives reads, authenticated likes and
creates. The harness supplies multiple local source IPs so source-port exhaustion
does not artificially cap the generator. It saves raw k6 summaries and records
the server image, source and workload hashes, cgroup peak memory and OOM state.

A source-built **150,000-user, one-minute screening hold** with the initial
listen backlog failed: p95 **246 ms**, p99 **6,694 ms**, failed requests **6.56%**,
peak cgroup memory **1,626 MiB**, no OOM kill. This is a failed stress test, not a
score. The process listen backlog was subsequently increased to 4,096 to better
absorb connection bursts; this does not change the kernel configuration.

The next experiment batches pending writes on the next event-loop turn, with
at most 128 jobs per transaction. Every successful response waits for COMMIT;
individual foreign-key constraint failures remain isolated, and transaction
failures fail the whole batch. This is group commit permitted by rule 6.

| Candidate | Users | Hold | p95 (ms) | p99 (ms) | Errors | Result |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| Individual commits, backlog 4,096 | 100,000 | 5 min | 988 | 1,518 | 0.98% | Failed latency |
| Group commit | 100,000 | 1 min | 658 | 959 | 0% | Failed p95 |

These trials have different hold lengths; they do not establish a percentage
capacity improvement.

The earlier Node 24.15.0 run passed a **90,000-user five-minute hold** after
warm-up: **p95 377.4 ms**, **p99 830.1 ms**, **0 failed requests out of 3,346,076**,
and **1,734.9 MiB peak cgroup memory**, with no OOM kill.

After upgrading, Node 26.11.1 passed the same **90,000-user five-minute hold**:
**p95 323.9 ms**, **p99 732.6 ms**, **0 failed requests out of 3,348,999**, and
**1,764.3 MiB peak cgroup memory**, with no OOM kill. The two-minute 1,000-user
warm-up passed. This is a selected passing local load, not the maximum capacity
or an official droplet score. Runtime, image, source, workload, and raw k6
evidence are recorded in `performance-results.json`.

A subsequent Prettier formatting pass changed source layout only. The emitted
JavaScript AST matched the benchmarked version after excluding source-position
and comment metadata. All 42 official checks and the additional regression
checks passed again. The recorded benchmark source hash remains the hash of
the measured revision, before formatting.

## Correctness evidence

- On Node 26.11.1, `install.sh` and `test/run.sh` passed in a clean Ubuntu
  24.04 container, including the native binding build, TypeScript compilation
  and **42/42 official checks**. GitHub Actions requires upstream approval
  before its own jobs can run.
- Earlier Node 24.15.0 regression checks passed: fresh external database changes, invalid tokens,
  Unicode lengths, concurrent duplicate likes, empty JSON-typed like requests,
  mixed successful/failed batches, independently visible committed writes,
  acknowledged-write persistence across SIGKILL/restart and unchanged schema.
- On Node 24.15.0 at `nofile=256`, a fragmented Unicode upload finished while connection
  pressure rose; its complete 201 response closed gracefully and the committed
  post could be read back. Ordinary original-socket reuse passed after 66 seconds.
- TypeScript checking passed. These local checks do not establish remote CI or an official droplet score.

## Reproduction

From the submission directory, with the repository seed generated:

```bash
docker build -t twelve-fastify-qualification:local .
python3 benchmark.py --vus 90000 --label local-90000
```

This performs a 1,000-user two-minute warm-up, followed by a 60-second ramp to
the requested user count, a five-minute hold and the workload's 30-second
ramp-down. The warm-up has its own normal ramp-down before the scored run.
For a short diagnostic only, use `--warmup 0 --hold 1m`.

The limits and percentiles are evaluated by the original workload: p95 under
500 ms, p99 under 1,000 ms and failed requests below 1%. Ramps are included in
the exported aggregate, as in that script. Finding a local limit within 250
users and reproducing it on the official droplet are separate, unperformed steps.
