# TODO

Status: Cleanup complete. Strict tooling checks, all three native telemetry modes and official/HTTP checks pass.

## Target

One run format, a smaller load generator and dev CLI, and cheaper telemetry. Preserve the workload and exact latency results. Remove duplicated work and obsolete paths, rather than just moving code between files.

The experiment anchor is HEAD when a run starts. The first group starts at `b59374c`. It groups work since that commit; it does not identify the code measured by a reference image or a dirty build.

```text
src/runs/<anchor>/<run>/
  summary.json         # tracked results
  manifest.json        # local
  source.patch         # local tested changes
  raw/                 # local logs and profiles
```

Track result summaries only. Manifests, source patches, raw data and archive inventories stay local. Record the anchor, effective workload/instrument settings, status, observer checkout and verified build/source/binary identities for each variant. Keep unknown provenance unknown.

Capture staged and unstaged source changes and new build-input files against the measured base commit. Exclude run output and caches. Generate the patch and build from the same frozen inputs. Use image tags `<commit12>-<patch12>-<build12>`; record full hashes, build settings, image ID and binary hash in the manifest. The build hash distinguishes targets, telemetry modes, compiler options and toolchain/base-image versions. Each tested variant owns its patch; comparisons reference those runs.

Preserve the existing uncommitted telemetry changes and their results. Do not delete historical measurements during migration.

## Work, in order

- [x] Create the goal and audit infrastructure with three agents
- [x] Approve this scope before implementation

### 1. Results and provenance

- [x] Give benchmarks, profiles and native probes one run directory and summary/export path. Stop appending new runs to a global results array; keep perf analysis separate from the final summary
- [x] Capture the tested source patch including new build-input files, verify reconstruction against its base commit, and build/tag from those same frozen inputs. Keep patch identity separate from build configuration and verify image/binary identity
- [x] Group the current telemetry probes under `b59374c`. Preserve historical results under `legacy` with original paths and metadata; do not relabel them as new runs
- [x] Migrate raw runs without losing data, update our links/ignore rules, and keep benchmark C++ sources separate from outputs
- [x] Verify successful, failed and interrupted runs, variant identities, instrument settings, archive rendering and unsupported-format errors

### 2. Dev tooling and builds

- [x] Keep `check`, `bench` and `results`. Replace `profile`/`--scope` with `bench --perf off|user|kernel|all`; default to 5k users with perf, otherwise 350k. Keep 30s ramp and 60s hold defaults
- [x] Select server telemetry, client diagnostics and perf independently. Normalize settings once, use one workload lifecycle and one summary writer
- [x] Remove host `--k6` and custom `--save` destinations. Keep pinned Docker k6 as the reference. Replace misleading `--no-build` with explicit server-image reuse; always check native-generator freshness
- [x] Configure generator builds with only llhttp and yyjson. Share dependency pins and remove unused server/probe inputs from the generator build and fingerprint
- [x] Fix stale `src/checks` editor paths
- [x] Reuse the tested server for HTTP checks only if flags and binary identity match. Keep required submission scripts and active editor support
- [x] Pass CLI/default/error tests, instrument-combination tests, strict Python checks and targeted build checks; persist generator errors before final collection
- [x] Simplify runtime ownership and report evaluation, remove duplicated parsing and plumbing, and keep the default result view compact. Check whether the server export needs changes
- [x] Remove repeated image verification and generator control logic, use normalized workload settings directly, and commit the completed tooling separately

### 3. Native load generator

- [x] Separate CLI/input handling, runtime ownership, metrics/reporting and native tests. Move socket fixtures to a test executable and replace production `--self-test` with CTest
- [x] Give runtime only settings it uses. Remove unused `Request::received` and the duplicate connecting assignment
- [x] Replace three copies of each latency sample with one exact store carrying endpoint and completion phase. Avoid copying old samples during growth; aggregate and sort after measurement
- [x] Correct client clock metadata without changing timestamps. Preserve RNG draws, timer ordering/budgets, HTTP validation, failed/timeout/canceled sample rules, interpolation, sorted-order means and exit codes
- [x] Pass moved conformance tests and focused old/new sample equivalence cases, then compare a short fixed-seed workload and memory footprint

### 4. Telemetry hot paths

- [x] Derive histogram counts from buckets when reporting: keep the smaller histogram and measured record-path improvement
- [x] Test cumulative DB counters with interval deltas: rejected because the controlled probe did not isolate a benefit
- [x] Reject CQ snapshot reuse after the controlled comparison; keep the split hooks that remove the basic-mode send call. Preserve receive/send/retry/shutdown coverage
- [x] Compare hot-path candidates separately with equal native-probe sample counts and unchanged clocks; preserve retained and negative results under `src/runs/`

### 5. Telemetry storage and cold paths

- [x] Fill history slots directly to remove temporary snapshots/copies; preserve chronology, totals, wrap and loss accounting
- [x] Keep demand-driven history storage after the startup/RAM and saturated-capture comparison; preserve dynamic ring growth and 128-window capacity
- [x] Remove redundant WAL-advice initialization/copies and stop constructing events that cannot be retained, preserving aggregate and dropped counts
- [x] Keep worker ownership, sync forwarding, errno and existing clocks intact; preserve counts in focused tests and the controlled comparison
- [x] Pass all three native modes, histogram edge cases, ring-growth/history-wrap and telemetry-export tests

### 6. Finish

- [x] Update concise usage docs and links; retain the JSON reader for older experiment formats
- [x] Pass strict tooling checks, 36 native tests across three modes, 42 official checks and 19 HTTP regressions. Record short comparisons without large load tests
- [x] Review the final diff and remaining TODOs; verify archived data and source patches. Commit and push only when requested

## Ownership

- Main: run layout/provenance, CLI decisions, shared CMake/Docker integration and acceptance
- Generator agent: `src/load/` and generator tests
- Tooling agent: `src/devtools/`, CLI tests and result migration after the run contract is fixed
- Telemetry agent: collectors, histograms/history and focused tests, after results can record comparisons

Assign disjoint files before each implementation batch. Agents preserve concurrent changes; main owns cross-module edits and final checks.

## Already done

- [x] Keep large snapshot frames out of per-turn deadline checks
- [x] Cache the DB scope histogram, reducing the scope from 64 to 56 bytes; retain measurements and rejected inlining results

## Next

- [x] Optimize the single-worker generator: prepare sockets before the start gate and reuse immutable request bytes, preserving HTTP checks and workload timing
- [x] Validate generator CPU headroom with repeated short 350k runs, the 100ms activation margin and exact latency aggregation; one worker has sufficient reserve
- [x] Check probabilities and think times against `bench/load.js`; exclude client preparation from HTTP duration and begin think time after response processing
- [x] Compare CQ waits with focused I/O tests and a short 350k A/B; keep `wait_nr=1`
- [ ] Reduce wait-one CPU costs without losing write batching or tail latency; stay compatible with stock Linux 6.8
- [ ] Compare direct B-tree post reads with prepared SQL, including the complete request lifecycle and fallback behavior

## Later

- [ ] Check socket/heap/pool memory after draining, then allocation reuse and socket-buffer caps in Ideas.md order
- [ ] Split foreground COMMIT stalls into sync/write/retry time and waiting before receive service; compare multishot receive afterward
- [ ] Test capacity on comparable Linux hardware with a separate generator and the official file limit
- [ ] Finally move our source/docs into `submissions/<stack>-SmolBlackHole/` and rename SUBMISSION.md to README.md

Keep our code in `src/`, use English and strict typing, and leave upstream files and other submissions alone
