# Five external workers with four stock generators

This local sensitivity measures whether sharing external workers limits the four-generator single-queue TAP fixture. It changes the relay CPU list from `2,5,8,14` to `2,5,8,9,14`. Each generator’s transmitted frames and returned replies are handled by the same dedicated worker on cores 2, 5, 8 and 9 respectively. CPU 14 reads the service TAP and produces the four return queues. The service and its threaded NAPI continue to share CPU 10. The manager stays on CPU 15. Stock mload, generator CPU groups, one-way delay, MTU 1500, 4,096-frame TAP rings, offloads, private TCP hash and service memory limit match the four-worker profile.

The separate topology is `single-queue-split-polling-tap-external-txring4096-five-workers`. Adding an external worker also changes contention between relay threads injecting packets into the single service TAP. Compare matched runs within each profile.

## Prepare

Use a Linux checkout with the primary benchmark’s private TAP permissions and dependencies, GNU patch and a C compiler. From the repository root, set `study` to the measurement workspace which owns the shared `cpu9-bench.lock`. For an independent workspace, create that ordinary lock file once. The output directory must be fresh.

```sh
official=submissions/c-uring-willwang/tests/official
python3 "$official/five-worker/prepare.py" --output "$study/five-worker/fixture"
```

The preparer draws on the shared fixture sources already in this checkout. It reconstructs the fingerprinted collector and both the historical and scheduler-aware assessors in a private directory. The historical relay source is included to reproduce the exact executable. GCC 13.3.0 on Ubuntu 24.04 reproduced its full ELF SHA-256, including debugging information, in two independent build paths. A different toolchain may instead supply the exact original ELF with `--archival-relay <path>`. Both modes check every source and the executable before publishing the fixture. [Frozen manifest](manifest.json)

| Artifact | SHA-256 |
|---|---|
| Five-worker manifest | `79fcebf1e402295ab437ae86dbaa0a63a667d7d5c0f2d1ee18193a27f6bec606` |
| Original relay C source | `11c949f7105382c8bf06f7d974fe68371f178f2d8b9e959a8f662360ab781236` |
| Original relay executable | `54935ba0c1222b0f2a1267e051c84c7d6c07bfbf1be070121be541e28e58c4f1` |
| Organizer load generator | `48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6` |

## Run the low-rate control

Set `service`, `service_cwd` and `mload` to absolute paths for the selected executable, its working directory and the compiled organizer generator. From the repository root, run as the ordinary benchmark user.

```sh
python3 -B "$study/five-worker/fixture/five-worker/run_five.py" \
  --server "$service" --cwd "$service_cwd" --mload "$mload" \
  --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
  --router-cpus 2,5,8,9,14 --manager-cpu 15 \
  --users 128 --ramp 36 --hold 44 --tag five4096-control-128 \
  --output "$study/five-worker/runs"
```

The launcher owns the primary measurement’s component lock and the existing router mutex through cleanup. A second outer `quiet_run.py` invocation would contend with its own lock. For a scored-size comparison, use the exact primary binary, working directory, original inputs, requested user count and hold. Omitting `--ramp` uses the primary stock ramp formula. The raw score, lock, source fingerprints, verified TIDs and handoff assignments are recorded under the specified tag. A fresh `--wire-proof-only --output <directory>` runs just the four-lane preflight.

The existing 128-user smoke held all 128 connections at all seven expected ticks. It completed 911 HTTP requests with zero failures, p95 0.759 ms and p99 2.157 ms. All four lanes independently verified 86,296 bytes at MSS 1448 on receive CPU 10. Every observed native interface, relay and return queue recorded zero drops. This establishes low-rate placement and integrity. A high-load result requires the separate matched control.

To reassess a completed archive, run `python3 -B "$study/five-worker/fixture/five-worker/assess_five.py" <run-directory>`. The shared assessor checks the stock result and external health. The five-worker checks verify actual affinities, stable task identities, lane ownership, packet integrity and both phases’ coverage.
