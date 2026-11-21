# Ordinary RPS on the 4,096-frame split TAP

This local calibration uses ordinary Linux TCP. Device NAPI and initial GRO work run on external TAP injection CPUs, then a private one-bit RPS map sends hashed service TCP packets to CPU 10's receive backlog. The comparison retains the stock scorer, four external clients, 75 µs each way, private TCP hash, offloads, queue geometry, single TX reader and four reply handoffs. Moving device work can improve apparent capacity. RPS also adds cross-CPU backlog and IPI costs. The difference from threaded NAPI combines these effects and does not measure a standalone scheduler percentage or organizer capacity.

The preparation script reads 14 files from the challenge checkout and checks their combined fingerprint against reference revision `7cf6b912279a283932ba1c3a495acf811c6bb099`. A source archive or squashed checkout works. The script changes the private service receive setup and selects its assessor in the temporary copy. [Shared Linux prerequisites](../tap-split/README.md) · [Source and executable provenance](../tap-split/relay-lifecycle-proof.json)

```sh
checkout=$PWD
official="$checkout/submissions/c-uring-willwang/tests/official"
work="$HOME/twelve-ordinary-rps"
python3 "$official/softirq-rps/prepare.py" --checkout "$checkout" --work "$work"
export TWELVE_SPLIT_TAP_RELAY="$work/frozen/tap-split/relay"
entrypoint="$work/frozen/softirq-rps/run_rps.py"
python3 "$entrypoint" --wire-proof-only --output "$work/four-lane-proof"
```

The default build uses the checkout's orphan-safe relay; GCC 13.3.0 on the reference Ubuntu toolchain produces ELF `0444da76098fba2d7cc3c664a2f3e8bf296d597cb9c3e633eaea5776f86ea8c4`. The archived comparison used `54935ba0c1222b0f2a1267e051c84c7d6c07bfbf1be070121be541e28e58c4f1`. To reproduce the historical launch identity, supply the existing historical executable via `--archival-relay /absolute/path` and export that same path in `TWELVE_SPLIT_TAP_RELAY`. Each preparation saves the actual ELF, source and manifest fingerprints in `provenance.json` and the runtime shape. A rebuilt relay with a different hash belongs in a separately labeled reproduction cohort. Preparing both modes from a source archive reproduced each of the 16 runtime source hashes, the relay executable, and the full manifest recorded for that mode. Changing a single byte in a shared source stopped preparation before the workspace was created.

A real zero-packet CPU 9 setup read the new private TAP mask as 0200 twice, its original and loopback masks as 0000, threaded mode as 0, and the private TCP hash as 16,384. Earlier single-flow traces observed all captured service TAP polls on injector CPU 15 in the ordinary runs and service CPU 9 in the threaded runs. Both placed socket receive processing on CPU 9. [Private queue measurements](queue-proof.json) · [Prior trace and kernel accounting](../tap-split/cpu-accounting.json)

In the four-generator smoke, the original PR 403 C server held 2,048 users at all eight expected samples of a 45-second hold. The organizer’s generator made 17,392 requests with zero failures and 1.751 ms p99. Before scoring, all four lanes delivered 86,296 byte-checked bytes each at MSS 1,448, with socket receive CPU 10. Seven ramp and six hold samples confirmed the private queue mask. The CPU 10 backlog recorded zero drops or time squeezes. [Stock score, byte counts and placement measurements](four-lane-smoke.json)

On the four-lane proof, require the following evidence before running a stock case.

- All five private interfaces have one RX/TX queue, MTU 1500, TX ring length 4096 and the original four offloads enabled. CPU 14 alone owns service TX. CPUs 2, 5 and 8 retain their four original handoffs.
- The private service queue keeps `threaded=0`, `rps_flow_cnt=0`, and CPU 10 alone in `rx-0/rps_cpus`. Each of four lanes uses one short-probe TCP socket and a separate byte-checked socket. Require receive CPU 10 and NAPI ID zero on all four short probes and 32 byte-checked exchanges per lane, with 86,296 bytes delivered per lane at MSS ≤1460. Socket ID zero does not identify the device NAPI CPU.
- Each stock run must contain a `softirq_rps_assessment` plus the ordinary scorer's full five-second occupancy, duration, cgroup and external-health/drops checks. A missing or changed queue readback, incomplete ramp/hold telemetry, mismatched source, changed TX handoff or failed packet proof excludes a result. The service CPU's backlog drop counter covers all traffic there.

Run trials through the serial controller on quiet hardware. Pass `--server`, `--cwd`, `--seed`, `--tokens`, `--mload`, `--users`, `--hold`, `--output` and `--tag` exactly as for the shared fixture. Start with a low-user stock smoke, then pair original PR 403 C and Kensac at 280k on the same disk. Check the original C 290k/120-second failure and Kensac 310k/300-second hold before optimized variants. Reassess with `python3 "$work/frozen/router/assess_rps.py" /path/to/run`.
