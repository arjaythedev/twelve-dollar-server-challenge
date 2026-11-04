# TODO

Status: Telemetry work is paused. Phases 1 through 3 are complete; the current task is runtime cleanup and separate checkpoints.

## Target

One run format, a smaller load generator and dev CLI, and cheaper telemetry. Preserve the workload and exact latency results. Remove duplicated work and obsolete paths, rather than just moving code between files.

The experiment anchor is HEAD when a run starts. The first group starts at `b59374c`. It groups work since that commit; it does not identify the code measured by a reference image or a dirty build.

```text
src/runs/<anchor>/<run>/
  manifest.json
  summary.json
  source.patch         # changes used by this tested variant
  raw/                 # ignored by Git
```

Track compact manifests, summaries and source patches. Keep detailed logs, telemetry and perf data in `raw/`. Record the anchor, effective workload/instrument settings, status, observer checkout and verified build/source/binary identities for each variant. Keep unknown provenance unknown.

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
- [x] Pass CLI/default/error tests, instrument-combination tests, strict Python checks and targeted build checks; preserve workload failures when collection or cleanup also fails
- [x] Simplify runtime ownership and report evaluation, remove duplicated parsing and plumbing, and keep the default result view compact. Check whether the server export needs changes
- [ ] Remove repeated image verification and generator control logic, use normalized workload settings directly, and commit the completed tooling separately

### 3. Native load generator

- [x] Separate CLI/input handling, runtime ownership, metrics/reporting and native tests. Move socket fixtures to a test executable and replace production `--self-test` with CTest
- [x] Give runtime only settings it uses. Remove unused `Request::received` and the duplicate connecting assignment
- [x] Replace three copies of each latency sample with one exact store carrying endpoint and completion phase. Avoid copying old samples during growth; aggregate and sort after measurement
- [x] Correct client clock metadata without changing timestamps. Preserve RNG draws, timer ordering/budgets, HTTP validation, failed/timeout/canceled sample rules, interpolation, sorted-order means and exit codes
- [x] Pass moved conformance tests and focused old/new sample equivalence cases, then compare a short fixed-seed workload and memory footprint

### 4. Telemetry hot paths

- [ ] Derive histogram counts from buckets when reporting instead of updating a redundant count per sample
- [x] Test cumulative DB counters with interval deltas: rejected because the controlled probe did not isolate a benefit
- [ ] Reuse the reactor CQ snapshot and remove basic-mode send hooks that do nothing. Preserve acquire ordering and test receive/send/retry/shutdown paths
- [ ] Compare candidates separately with equal native-probe sample counts and unchanged clocks. Freeze generator, seed, settings and limits for workload pairs; record negative results and discard inconclusive changes

### 5. Telemetry storage and cold paths

- [ ] Fill history slots directly to remove temporary snapshots/copies; preserve chronology, totals, wrap and loss accounting
- [ ] Test demand-driven history storage with dynamic ring growth and the same 128-window capacity. Compare startup/RAM and periodic capture costs separately
- [ ] Remove redundant WAL-advice initialization/copies and stop constructing events that cannot be retained, preserving aggregate and dropped counts
- [ ] Keep off/basic/profile contracts, worker ownership, sync forwarding and errno intact. Do not merge timestamps, remove CPU clocks or introduce sampling in this cleanup
- [ ] Pass all three native modes, histogram edge cases, ring-growth/history-wrap and telemetry-export tests

### 6. Finish

- [ ] Update concise usage docs and links. Remove obsolete files/readers only after retained data has a supported path
- [ ] Run strict tooling checks, relevant native tests and official/HTTP checks once integrated. Use short comparisons, no large load tests
- [ ] Review the final diff and remaining TODOs. Commit and push only when requested

## Ownership

- Main: run layout/provenance, CLI decisions, shared CMake/Docker integration and acceptance
- Generator agent: `src/load/` and generator tests
- Tooling agent: `src/devtools/`, CLI tests and result migration after the run contract is fixed
- Telemetry agent: collectors, histograms/history and focused tests, after results can record comparisons

Assign disjoint files before each implementation batch. Agents preserve concurrent changes; main owns cross-module edits and final checks.

## Already done

- [x] Keep large snapshot frames out of per-turn deadline checks
- [x] Cache the DB scope histogram, reducing the scope from 64 to 56 bytes; retain measurements and rejected inlining results

## Later

- [ ] Check socket/heap/pool memory after draining, then allocation reuse and socket-buffer caps in Ideas.md order
- [ ] Split foreground COMMIT stalls into sync/write/retry time and waiting before receive service; compare multishot receive afterward
- [ ] Test capacity on comparable Linux hardware with a separate generator and the official file limit
- [ ] Finally move our source/docs into `submissions/<stack>-SmolBlackHole/` and rename SUBMISSION.md to README.md

Keep our code in `src/`, use English and strict typing, and leave upstream files and other submissions alone
