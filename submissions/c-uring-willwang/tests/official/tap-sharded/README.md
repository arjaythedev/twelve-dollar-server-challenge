# Four-worker TAP control

This profile preserves one service TAP receive queue, one transmit queue and a single NAPI task pinned to the application CPU. Each private TAP is read by one external worker. The service TAP reader also writes service replies into the four destination Linux TCP stacks. That last step made the reader a bottleneck in a 280,000-user control. The [split relay](../tap-split/README.md) gives those writes to destination workers and keeps the same service network interface.

## Reproduce the control

Use the Linux prerequisites, stock mload and seed from the common setup. The four-worker defaults reserve CPUs 2, 5, 8 and 14 for the switch, CPU 10 for the application and its private NAPI task and 75 µs of one-way delay. Both commands below support overriding CPU placement and link delay. [Shared fixture setup](../router/README.md)

```sh
fixture="$PWD/submissions/c-uring-willwang/tests/official/tap-sharded"
work=/absolute/path/to/benchmark-work
entry=/absolute/path/to/submission
mkdir -p "$work"
export TWELVE_SHARDED_TAP_RELAY="$work/sharded-tap-relay"
cc -O3 -g -Wall -Wextra -Werror -std=gnu11 -pthread \
    "$fixture/relay.c" -o "$TWELVE_SHARDED_TAP_RELAY"

python3 "$fixture/run_sharded.py" --wire-proof-only --output "$work/sharded-wire-proof"
python3 "$fixture/run_sharded.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
    --users 280000 --hold 120 --output "$work/runs" --tag sharded-control-280k
```

The five-TAP proof delivered 345,184 byte-checked TCP response bytes and 80 ICMP echoes, with lane median RTTs of 0.242–0.281 ms, 52 GSO frames, 416 partial-checksum frames and zero interface or switch drops. Under full load the service transmit ring accumulated 20,870 drops by about 86 seconds while its qdisc reported zero drops. One eight-second ramp interval had 88.1% idle scans but 93.8% of wall time spent on productive passes. The worker delivered 34.9% of frames more than 250 µs past their deadline in that interval. Drops disqualify this control as a fair score comparison. [Route proof](wire-proof.json) · [FIFO tests and binary verification](VALIDATION.md)
