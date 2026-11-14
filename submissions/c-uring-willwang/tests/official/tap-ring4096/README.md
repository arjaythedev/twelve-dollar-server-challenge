# Split TAP with a 4,096-frame native transmit ring

This local sensitivity profile enlarges the native transmit ring on each of the five private TAP devices from 1,000 to 4,096 entries. It uses the same single guest RX/TX queue, threaded NAPI, four external relay workers, packet order, 75 µs one-way delay, ordinary Linux TCP, address aliases, service limits and stock scorer as the 1,000-entry split profile. The runner checks every device's original capacity and ifindex before and after changing the ring in its private namespace. The benchmark service keeps the same transient one-core affinity as every entry. [Parent split fixture](../tap-split/README.md)

The original fixture observed 1,839 service transmit drops in one interval of the 300,000-user bank control, with zero qdisc or relay FIFO drops. At 38,312 service frames/s, its 1,000-entry ring could fill in 26 ms. The external reader accumulated 98.3 ms of runnable wait during those 8.2 seconds, compared with a median 5.1 ms in other intervals without native drops. The longest individual pause was unmeasured. A queue of 4,096 is the first power of two above 1,000 plus 1,839 entries. Each entry holds one Linux packet buffer, which can contain multiple TCP segments when GSO is active. The larger ring permits a longer external stall and can add queueing delay during sustained saturation. The organizer's effective NIC queue capacity is unknown. [Measured controls](queue-proof.json) · [Linux TUN transmit queue](https://github.com/torvalds/linux/blob/v6.8/drivers/net/tun.c#L1125) · [Linux TUN ring resize](https://github.com/torvalds/linux/blob/v6.8/drivers/net/tun.c#L3661)

## Reproduce

Follow the common router setup for the stock executable, tokens and disk seed. Set the paths below to existing files on the benchmark host. The relay executable can be shared with the 1,000-entry split runner, and the harness starts each submission's normal server binary. [Shared setup](../router/README.md) · [Relay binary verification](../tap-sharded/VALIDATION.md)

```sh
root="$PWD"
fixture="$root/submissions/c-uring-willwang/tests/official"
work=/absolute/path/to/benchmark-work
entry=/absolute/path/to/submission
mkdir -p "$work"
export TWELVE_SPLIT_TAP_RELAY="$work/split-tap-relay"
cc -O3 -g -Wall -Wextra -Werror -std=gnu11 -pthread \
    "$fixture/tap-split/relay.c" -o "$TWELVE_SPLIT_TAP_RELAY"

python3 "$fixture/tap-ring4096/run_buffered.py" --wire-proof-only \
    --output "$work/private-4096-wire-proof"
python3 "$fixture/tap-ring4096/run_buffered.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$root/seed/feed.db" --tokens "$root/seed/tokens.json" \
    --users 300000 --hold 300 --output "$work/runs" --tag ring4096-candidate-300k
python3 "$fixture/tap-ring4096/loss_window.py" "$work/runs/ring4096-candidate-300k"
```

The scored topology is `single-queue-split-polling-tap-external-txring4096`. Compare candidates and controls that report this same topology, CPU assignments, schedule and health criteria. `shape.json` records the executed relay ELF hash, launch-time digests for twelve Python setup and observer files, and every queue's previous and final geometry. `external-loss-timing.json` locates native device loss between two scored observations and includes the three fully preceding and following stock five-second windows with synchronized participants and per-shard p99. The merged score remains available in `summary.json`.

The four-worker route proof read back one RX queue, one TX queue, 1,500-byte MTU and a 4,096-entry transmit ring on all five devices. It transferred 345,184 checked TCP response bytes with an MSS of 1,448. Each lane returned all 20 ICMP probes, with median RTTs of 0.232–0.248 ms. All 297 service reply handoffs completed, including broadcast recipients. The relay counted 52 GSO frames, 415 partial-checksum frames, zero queue-full events and zero drops across all five interfaces. The captured twelve-file source manifest matches local revision `e73be93`. The current assessment also checks the recorded scheduling delays of the four external workers. [Four-core proof and source fingerprints](queue-proof.json) · [External worker measurements](../tap-split/relay-lifecycle-proof.json)

## Check the native driver capacity

The independent capacity check starts a private switch with its reader held idle. An ordinary process sends exactly 100 more IPv4 frames than the chosen queue length. A sender-scoped `perf` trace records the kernel's free reason and driver function. The 1,000 and 4,096 controls each produced exactly 100 `tun_net_xmit` frees with `FULL_RING`, matching the device TX counter. Both qdiscs forwarded all attempted packets with zero qdisc drops. Use two spare cores and run these low-rate tests separately from a scored trial. [Recorded driver and four-lane route checks](queue-proof.json)

```sh
export TWELVE_TAP_RELAY="$work/held-test-relay"
cc -O2 -Wall -Wextra -Werror "$fixture/tap-poll/relay.c" -o "$TWELVE_TAP_RELAY"
python3 "$fixture/tap-ring4096/verify_capacity.py" "$work/control-1000" \
    --txqlen 1000 --sender-cpu 9 --relay-cpu 15
python3 "$fixture/tap-ring4096/verify_capacity.py" "$work/control-4096" \
    --txqlen 4096 --sender-cpu 9 --relay-cpu 15
```
