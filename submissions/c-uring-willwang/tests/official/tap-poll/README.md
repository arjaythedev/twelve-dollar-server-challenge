# Single-queue TAP benchmark

The four stock Go generators communicate with the original Linux service through a private Ethernet switch. The server owns one TAP transmit queue and one receive NAPI thread pinned to its application CPU. An external CPU continuously reads the TAP queues without registering a blocking waiter. Outbound service packets enter the external queue without waking a remote router CPU. Both sides retain the kernel TCP/IP stack, the negotiated MSS and the 1,500-byte link MTU.

## Reproduce

Follow the [router fixture setup](../router/README.md) for the stock client, seed, memory limit, CPU selection, ramp, hold and eligibility checks. This variant also requires a C compiler, Linux TUN/TAP with NAPI support, a mounted `/dev/net/tun`, and Linux `perf` for the optional microtrace. The local setup process creates only transient devices in the new test namespaces. It drops its root identity and supplementary groups before forwarding frames. The submitted server and stock clients run under the original benchmark user.

```sh
fixture=$PWD/submissions/c-uring-willwang/tests/official/tap-poll
work=/absolute/path/to/benchmark-work
entry=/absolute/path/to/submission
mkdir -p "$work"
export TWELVE_TAP_RELAY="$work/tap-relay"
cc -O2 -Wall -Wextra -Werror "$fixture/relay.c" -o "$TWELVE_TAP_RELAY"

# Two private endpoints, 128 byte-checked replies, task-scoped IPI and NAPI traces.
python3 "$fixture/proof.py" --app-cpu 9 --remote-cpu 15 --out "$work/tap-ipi-proof"

# Check the four private generator routes with 16 TCP exchanges and 80 ICMP packets.
python3 "$fixture/run_tap.py" --wire-proof-only --app-cpu 9 --relay-cpu 15 \
    --output "$work/tap-route-proof"

# A normal scored trial accepts the same arguments as router/run.py.
python3 "$fixture/run_tap.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
    --users 280000 --hold 300 --output "$work/runs" --tag tap-candidate-280k
```

The external relay uses the last of the four `--router-cpus` values, CPU 14 by default. The other three remain reserved for a matched comparison with the veth fixture. The proof commands use separate spare cores and take a per-user, per-CPU proof lock. Set `TWELVE_WIRE_PROOF_LOCK` to share an existing scratch benchmark lock. All scored profiles take the same per-user trial lock.

## What the wire measures

- The helper opens one nonpersistent TAP per endpoint with an Ethernet header and a virtio-network header. It verifies that every namespace is distinct from the host and the other endpoints. Linux preserves its normal TCP checksum and segmentation metadata across these FDs. The relay validates checksum bounds and GSO metadata, forwards Ethernet frames to the matching private MAC, and floods broadcast frames for normal ARP.
- The service TAP registers one threaded NAPI context through the Linux 6.8 TUN driver. The driver uses `netif_napi_add_tx` for its userspace injection path, so `SO_INCOMING_NAPI_ID` is zero. The fixture gives this private TAP a unique name, verifies its namespace and kernel-device type, and checks that enabling `threaded` created exactly one new kernel task with that name. It verifies the task's CPU affinity and the actual established sockets' `SO_INCOMING_CPU` values. The microtrace measures `napi:napi_poll` in that task and records the actual device, CPU, polls and work. [Linux 6.8 tun_napi_init](https://github.com/torvalds/linux/blob/v6.8/drivers/net/tun.c#L265) · [Linux 6.8 NAPI registration](https://github.com/torvalds/linux/blob/v6.8/include/linux/netdevice.h#L2729)
- The switch applies a 90 µs one-way delay from the time it reads a frame. Every source records its actual unloaded and loaded RTT. Queue capacity, forwarding lateness, partial checksum and GSO frame counts, idle scans, and every TAP's RX/TX errors and drops are recorded alongside the stock histograms. The dedicated relay polls continuously, so its CPU reads as busy when there is spare forwarding capacity. A sample interval with fewer than 1% idle scans and more than 1% of frames over 250 µs late flags saturation. Forwarding failures, invalid frames, queue overflow and TAP drops also flag the trial. Network counters observed before the score cutoff are reported separately from socket teardown.

In the 128-response microtrace, three veth trials generated 122, 128 and 129 remote service TCP TX IPIs. The corresponding TAP trials generated zero. The final trial forwarded 51 GSO frames and 321 partial-checksum frames. Both participants verified the same 360,160 data bytes and recorded zero checksum errors. The final TAP trial recorded two server retransmissions and zero relay drops. The final NAPI trace located all 183 service TAP polls on the application core. All five interfaces reported one RX queue, one TX queue, 1,500-byte MTU and a 1,000-frame TX queue limit. Two four-client route tests used 90 µs delay on the shared scratch CPUs. Lane median RTTs were 0.260–0.269 ms on one test and 0.696–1.360 ms on the other. The largest individual outlier was 38.8 ms. Each scored trial repeats these measurements using its dedicated external relay core and separate client cores. [Recorded proof](wire-proof.json)

`host_checks.tap_drop_attribution` separates receive errors and drops reported by the service TAP from the service’s transmit queue, the four generator interfaces, and the relay’s internal FIFO and writes. A failed relay write directed to the service carries an undetermined cause. It remains an external qualification flag. A pure service-side kernel RX drop appears in `target_network_flags`. Strict TAP `qualified` still requires clean target RX counters. `scored_healthy_environment` evaluates the external path and allows the service’s own receive behavior to be examined alongside its stock score.

TAP is a local measurement fixture. The guest and load generators still share a kernel, and a software NAPI worker and software switch approximate the organizer’s virtual NIC. Use the known official pass/fail boundaries to decide whether the profile reproduces competitive ranking.
