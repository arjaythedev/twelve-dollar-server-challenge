# Relay build and validation

The two calibration binaries were built on x86-64 Ubuntu 24.04 with GCC 13.3.0 (`13.3.0-6ubuntu2~24.04.1`), GNU Binutils 2.42 and glibc 2.39 (`2.39-0ubuntu8.9`). The following commands run from `tests/official`. They produce runnable binaries in a temporary directory and verify both SHA-256 hashes.

```sh
work=$(mktemp -d)
for variant in tap-sharded tap-split; do
    gcc -O3 -g -Wall -Wextra -Werror -std=gnu11 -pthread \
        "$variant/relay.c" -o "$work/$variant.debug"
    objcopy --strip-all --remove-section=.note.gnu.build-id \
        "$work/$variant.debug" "$work/$variant"
done
(
    cd "$work" || exit 1
    sha256sum --check <<'HASHES'
afd74e5d92d63557a565d3170845ea9fb7c67bac98687215561077dabb1fc1e0  tap-sharded
1793119a56410fee34dcc36d647eb0351f0ec11ae8e1174d273c98ed800c9d24  tap-split
HASHES
)
```

The original experiment kept debug info. Moving and formatting the source changes the debug records and GNU build ID. The archived debug ELF digests were `f46881d1bbd568f9154e2314c73ae4150713549ed4052edbfd23ac98ea9aafe4` for sharded and `54935ba0c1222b0f2a1267e051c84c7d6c07bfbf1be070121be541e28e58c4f1` for split. To rebuild the split relay used in the scored comparisons, use `tap-split/relay.c` at local Git revision `e73be93`. Applying the same `objcopy` command gave `a988745d8e79685ef1db73b5decf35124da6da0cdcb1c0ffd2a41c8863849cd5` for both its build and the archived scoring ELF. The current split relay also exits when its launcher dies. [Control pipe tests](../tap-split/relay-lifecycle-proof.json)

## FIFO and buffer ownership

Both local regressions use nonblocking Unix datagram socket pairs. They run as an ordinary user and exercise the same C forwarding functions used on a TAP. Run the following from the same shell as the build above.

```sh
gcc -O2 -g -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    tap-sharded/fifo-test.c -o "$work/fifo-test"
gcc -O2 -g -std=gnu11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    tap-split/split-test.c -o "$work/split-test"
UBSAN_OPTIONS=halt_on_error=1 "$work/fifo-test"
UBSAN_OPTIONS=halt_on_error=1 "$work/split-test"
```

The sharded test fills a destination send buffer, then verifies every resumed write in FIFO order. It checks configured delay, all four broadcast copies, virtio GSO bytes, malformed-frame accounting and an unread kernel frame preserved behind a full 2,048-entry userspace FIFO. The split test holds one broadcast destination blocked while the other three release their copies, checks the shared slot survives until the final consumer returns it, then fills a destination FIFO and checks its pending successor. Four concurrent consumers also verify 320 successive payloads, lengths and orders. In a ptrace-based sandbox, use `ASAN_OPTIONS=detect_leaks=0` for the two executions because LeakSanitizer cannot enumerate threads there.

## Private TAP checks

The full five-TAP preflight verified 345,184 bytes over four simultaneous TCP streams, the single guest queue and each ordinary worker's observed TID and CPU affinity. The split fixture supports the same checks with one external CPU. The two namespace launchers and their recorded results are described in the fixture READMEs. [Sharded fixture](README.md) · [Split fixture](../tap-split/README.md)

The first sharded calibration lost frames on the service TAP after the qdisc. Its single service reader also performed all four client injections. In one eight-second interval 88.1% of its polling iterations were idle, yet productive iterations took 93.8% of elapsed time. The link's TX-drop count rose from 3,605 to 20,870. A private IPv4 control then offered 1,100 packets to the same 1,000-slot TAP with its reader paused. The sender recorded 100 `tun_net_xmit` drops with reason `FULL_RING` and zero fq_codel drops.

The split reader handed replies to their destination workers. Across 294.3 measured hold seconds of the first 300,000-user calibration, the guest TAP read 47,269 TX frames/s and accepted 69,882 RX frames/s with zero device drops. The four 2,048-entry response queues peaked at 281, 284, 192 and 280 entries during measurement. Each had zero full-queue polls, write retries and drops. After teardown all four queues drained, and their combined enqueue and delivery counts both reached 18,249,555. This calibration established headroom in the external relay. Application eligibility is assessed separately by the scorer.
