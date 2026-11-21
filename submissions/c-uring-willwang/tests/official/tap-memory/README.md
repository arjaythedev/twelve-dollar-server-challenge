# A local RAM sensitivity test for passive TCP sockets

An end-to-end check ran 2,048 stock users through the four generators and the unmodified C executable. It completed 9,743 requests with zero errors, 0.68 ms p95 and 2.97 ms p99. The manager recorded 700 samples inside the scoring window, observed 2,048 live sockets and reported a 102.44 ms p99 sampling interval. This check exercises the complete runner at its original leaf limit. The scaled tests below force reclaim and a contained OOM. [Scored trial and manager observations](../capacity/local-results.json)

This optional harness runs the four stock mload generators, server executable, private single-queue TAP and CPU assignments from the ring4096 fixture. A dedicated, transient cgroup bounds the collective cost of the application and a measured amount of TCP kernel memory that the application cgroup misses. The application keeps its original 1,750 MiB limit, zero swap, file limit, seed and environment. Run each contender with a fresh instance and a distinct result tag. The profile name is `single-queue-split-polling-tap-external-txring4096-private-passive-tcp-memory`.

Two local measurements on Ubuntu's 6.8.0-147-generic kernel established the adjustment. All 3,072 completed passive IPv4 handshakes in one order and all 1,536 in the reverse order added zero bytes to the server's measured slab charge before accept. Accepting added 1,304 bytes per connection. Creating sockets in the application's process context charged 3,680 bytes each at matched descriptor-table occupancy. The difference, 2,376 bytes per socket, equals the current TCP slab slot (2,368) plus its objcg entry (8). Packet buffers and ordinary accepted-file costs that reach the application's memory counter already consume its budget. [Measured source digests](calibration.json) · [Paired socket measurements](../capacity/passive-socket-memory.json) · [Linux IPv4 socket counters](https://github.com/torvalds/linux/blob/v6.8/net/ipv4/proc.c#L51)

Every 100 ms an external process counts full IPv4 TCP sockets from the service namespace's `TCP: inuse` field in `/proc/<service-pid>/net/sockstat`. It subtracts the verified IPv4 listeners. The kernel counts complete connections waiting for accept and some closing states. It accounts for TIME_WAIT and half-open requests separately. The monitor verifies that the namespace makes no new active TCP opens. It changes only the transient parent’s `memory.max`, rounding down to the kernel page size.

```text
parent cap = min(1,750 MiB, max(0, 1,967 MiB - 2,376 bytes × full passive sockets))
```

| Live sockets | Cost outside the leaf cgroup | Maximum leaf workload under the parent |
|---:|---:|---:|
| 95,766 | 217.00 MiB | 1,750.00 MiB |
| 280,000 | 634.46 MiB | 1,332.54 MiB |
| 300,000 | 679.78 MiB | 1,287.22 MiB |
| 350,000 | 793.08 MiB | 1,173.92 MiB |
| 400,000 | 906.37 MiB | 1,060.62 MiB |

The 1,967 MiB envelope is an **upper bound on available guest RAM**. Once the socket budget binds it leaves no explicit reserve for the guest OS, other software, unusable slab tails, uncounted TCP objects, or uncharged kernel allocations. The host's TCP memory-pressure watermarks and global reclaim behavior also differ from a whole 2 GB VM. A competitor that reads host `/proc/meminfo` still sees the host. Reducing a cgroup limit on Linux 6.8 may do synchronous reclaim on the external writer's CPU or invoke that cgroup's OOM killer. A guest may pay equivalent work on its own CPU and select victims differently. This profile measures the sensitivity to one established discrepancy. [Kernel limit-write implementation](https://github.com/torvalds/linux/blob/v6.8/mm/memcontrol.c#L6801) · [Cgroup v2 memory controller](https://docs.kernel.org/admin-guide/cgroup-v2.html#memory)

## Running a matched control

Place the `tap-memory` directory next to the existing `tap-ring4096`, `tap-split`, `tap-sharded`, `tap-poll`, `threaded-napi` and `router` fixtures. Use the original mload SHA, built split relay and pristine database configured for the ordinary ring4096 profile. Run as the same unprivileged benchmark user. Substitute local paths for the example variables.

```sh
export TWELVE_SPLIT_TAP_RELAY="$artifacts/relay-split"
python3 "$fixtures/tap-memory/run_memory.py" \
  --server "$server" --cwd "$server_cwd" --users 300000 --hold 300 \
  --tag memory-guarded-300-h1 --output "$results" --mload "$mload" \
  --seed "$challenge/seed/feed.db" --tokens "$challenge/seed/tokens.json"
```

The existing fixture takes its normal exclusive trial lock and creates its usual namespaces. The optional wrapper asks systemd to place the server's unit in a fresh parent slice containing only that service. Before the stock clock starts, the manager verifies the service UID, actual ELF identity, private namespace inode, cgroup and affinities. It opens the single private limit file and telemetry handles, then permanently drops to the invoking user with zero effective capabilities and no new privileges. The monitor pins the main process with pidfd. No startup, deployment, kernel, global limit, or generator changes are required for a server entry.

The result directory includes `passive-memory.jsonl`, `service-controls.json` and `passive-memory-result.json`. Each sample records the observed count, cap, immediate pre/post-write usage, exact sample and write timings, parent/leaf memory events and pressure stalls, and reclaim counters. Samples bound the observed lag; connections can open or close between samples. The wrapper records genuine service OOM separately, marks early manager loss or a sampled gap over one second as an invalid fixture run, and leaves the original scorer metrics available. Read the final persisted `summary.json` after wrapper exit.

## Small proofs

The real-service proof uses a private, two-sided TCP wire with the server, external client and both private NAPI threads confined to leased CPU 9. It scales 1,750 and 1,967 MiB by exactly 1/64 and holds the measured per-socket cost fixed. It checks every connection's response against the exact expected bytes, then performs a write and immediate readback. A separate disposable allocation process verifies clean-cache reclaim, OOM attribution and survival of the external peers. Neither proof launches a scored mload run.

```sh
fixture="$fixtures/tap-memory"
cc -std=gnu11 -O2 -Wall -Wextra -Werror "$fixture/pressure_holder.c" -o "$artifacts/pressure-holder"
python3 "$fixture/test_monitor.py"
flock "$cpu9_lock" taskset -c 9 python3 "$fixture/prove.py" \
  --server "$guarded_server" --schema "$challenge/schema.sql" \
  --out "$results/private-real-proof" --work "$ext4_scratch"
flock "$cpu9_lock" taskset -c 9 python3 "$fixture/pressure.py" \
  --holder "$artifacts/pressure-holder" --out "$results/private-pressure-proof" \
  --work "$ext4_scratch"
```

The final real-service proof saw exactly 128, 2,304, 128 and zero live sockets. The parent cap fell from 28,672,000 to 26,750,976 bytes and recovered after close. The original leaf limit stayed fixed. All 2,338 HTTP responses passed, including the committed write/readback. Between confirmed client actions and an exact socket-count sample, the largest observed lag was 115.4 ms, including TCP close processing. The largest sampled controller interval was 107.6 ms. Its p99 sampling work was 0.346 ms and p99 cap write was 0.016 ms. The controller used 6.92 ms CPU during this short proof. The separately checked pressure process dropped from 28,430,336 to 26,333,184 charged bytes during a 0.676 ms limit write. Its controlled anonymous allocation then caused one parent-local OOM and one descendant kill. The client and manager survived, and all test slices and namespaces disappeared. [Proof and costs](proof.json)
