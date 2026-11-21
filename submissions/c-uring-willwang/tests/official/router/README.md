# Four-generator router benchmark

This fixture compares unchanged Linux servers against the organizer's stock Go load generator on a multicore Linux host. One dedicated physical core runs the service and receives its incoming TCP packets. Four private Ethernet paths carry traffic from four generators into that application's network namespace.

## Reproduce

Use Linux with systemd, cgroup v2 and swap accounting, iproute2, ethtool, ping, curl, util-linux, sudo and Python 3.11 or later. The host needs free memory for the four generators, the application and the host kernel. A trial also needs disk space for a fresh 238 MiB seed copy and its SQLite WAL. The benchmark user needs sudo access to create the private namespaces, configure their interfaces and create a transient system service. These privileges belong to the local test fixture. The submitted application and the benchmark's Go clients run as the calling user.

Build the entry using its published scripts. Generate the pristine seed and tokens, and build the vendored stock scorer with the submission's `tests/official/build-client.sh` and Go 1.27.3. The expected scorer SHA-256 is `48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6`.

```sh
# Run in the challenge checkout. Supply the location of this fixture and the entry.
fixture=/absolute/path/to/router
work=/absolute/path/to/benchmark-work
entry=/absolute/path/to/submission
mkdir -p "$work"
submissions/c-uring-willwang/tests/official/build-client.sh "$work"
python3 "$fixture/run.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
    --output "$work/runs" --users 280000 --hold 300 --tag candidate-280k-hold-1
python3 "$fixture/assess.py" "$work/runs/candidate-280k-hold-1"
```

Choose physical CPU numbers using `lscpu -e=CPU,CORE,SOCKET`. The tested defaults reserve CPU 10 for the application, 2/5/8/14 for the four router lanes, 15 for the driver and 0,1 / 3,4 / 6,7 / 11,12,13 for the four Go generators. CPU 9 is free. Override these with `--app-cpu`, `--router-cpus`, `--client-cpus` and `--manager-cpu`. The four comma-separated client lists are separated by semicolons. Each output tag must be new.

The runner serializes trials with a per-user lock. Before allocating any namespace or copying the seed it requires two one-second-spaced readings with fewer than 1,000 TCP orphans and 10,000 allocated TCP sockets across the host. `preflight.json` records the thresholds, initial and final counters, elapsed wait and eligibility. `preflight.jsonl` retains every reading. A 600-second timeout leaves an ineligible result without launching a scorer. Adjust `--max-idle-orphans`, `--max-idle-alloc` and `--quiet-timeout` for a host with a different measured baseline.

After the scorer finishes, the service and private links remain alive so TCP can close its sockets normally. `drain.jsonl` records the service state, the established connections in each endpoint namespace, and the host's allocated sockets, orphans and TCP memory. The runner requests service shutdown when all endpoints have closed, the service has exited, or two thirds of the `--drain-timeout` budget has elapsed. It also recognizes a drained client side when every generator has closed, the global idle thresholds hold, and a residual service count below the idle allocation threshold remains unchanged for six seconds. The links remain available for kernel teardown until counters settle or the 180-second default budget expires. `drain.json` records the shutdown reason and compares the final state with the pre-build idle reading. Only the scorer's original ramp and hold enter the latency histogram.

## Network and resource model

- Each of four client namespaces has a private Ethernet link to the router. The router has a second link for each lane into the **same server namespace**, where the original service binds a wildcard port 80. Each client uses eight server address aliases. Source ports stay at the host kernel's namespace defaults. The runner reads back the RX CPU masks, enables and checks GRO/GSO at both ends of each link, and uses a 1,500-byte MTU.
- The four router lanes have independent egress queues and RX CPUs. A 48 µs netem delay in each direction approximates the organizer's 0.25 ms unloaded RTT, including local forwarding and scheduler overhead. The run records each client's measured RTT. Under-load ICMP samples include queueing on the service core. Linux still forwards and receives all packets. The separate router cores model the external network path.
- The dedicated server namespace owns 16,384 established-TCP hash buckets. Linux 6.8 normally sizes this table at roughly one bucket per 128 KiB of RAM. The value models a 2 GiB guest. The official VM's actual hash size is unpublished. The router's private `tcp_child_ehash_entries` is set only while it creates this server namespace. The generator and host tables keep their own sizes. A `--server-ehash 0` trial measures the host-shared control. [Linux TCP initialization](https://github.com/torvalds/linux/blob/v6.8/net/ipv4/tcp.c#L4707) · [Kernel IP sysctls](https://docs.kernel.org/networking/ip-sysctl.html)
- A fresh systemd service gets a 1,750 MiB memory cgroup, zero swap, hard and soft file descriptor limits of 65,535 and only the port-bind capability. The seed copy is created inside that cgroup and checked by SHA-256 before the server runs. Fresh namespaces, seed copies and services are created for every probe and hold. The copied database and the transient system resources are removed at the end.
- Four stock scorer processes synchronize to one start timestamp, use disjoint virtual-user offsets, and preserve the published think times, request mix, 60 s request timeout and uncompressed responses. The default ramp is `max(60, ceil(3 × users / 10000))` seconds. This local approximation to the organizer's connection ramp was calibrated against published throughput. Use `--hold 120` for a probe and `--hold 300` for the score confirmation.

## Reading an outcome

`merged.json` contains the unchanged stock merge of all four full ramp-and-hold histograms. Its p95, p99 and error thresholds determine `stock_pass`. All four source histograms, endpoint counters and five-second timelines are kept.

`summary.json` additionally reports the requested virtual-user count and simultaneous live connections at corresponding five-second ticks on every shard. The window starts after the scorer's randomized 0–5 s initial sleep and ends at its final tick before shutdown. Local capacity confirmation requires the full target at every aligned tick, the full duration from every shard and no service OOM during the score. This is an additional check on the local generator. The stock pass and stricter local qualification are reported separately.

`raw_eligible` checks the stock target, all four shard durations and service OOMs. `all_scheduled_users_present` checks the actual user counts at every expected hold tick. `infrastructure_ok` requires usable external telemetry and clear external drop and saturation checks. `scored_healthy_environment` combines these three conditions and can be read alongside `stock_pass` when a few sockets took longer to connect. `wire_eligible` additionally checks simultaneous open connections. `qualified` requires the stock thresholds and the strict local connection and network checks.

The summary records app and router CPU, generator group CPU, pressure stalls, refaults, memory peak, host block-device and application-cgroup I/O, global TCP orphan counts, TCP retransmissions/timeouts, interface counters and router queue drops. The network snapshot samples at most 1,024 rows from the service namespace's socket table roughly every eight seconds to estimate its RX and TX queue direction. Router/generator saturation for a complete 30-second window at 98% or more, or packet drops on an external receive backlog or router egress queue, flags a fixture limit. Scored counters are sampled before cutoff. Final interface totals can include socket teardown. Raw observations include unmodified per-shard histograms, per-core snapshots, cgroup counters, router statistics and per-generator RTT logs. The app core may legitimately saturate. Each score is in concurrent virtual users, with throughput reported separately in requests per second.

This host shares its kernel, global memory accounting and disk with the generators. Its application sees host-wide `/proc/meminfo`. The official environment is a one-vCPU, two-gigabyte Intel VPS with remote generators. A run can validate the local ranking and independently test whether known official pass/fail boundaries reproduce.

Mojo pauses new accepts when `/proc/meminfo` reports less than 160 MiB available. In its 280,000-user comparison, the service logged zero pauses. At all 191 scored cgroup samples, Ubuntu Noble's lxcfs 5.0.4 formula would also have kept accepts enabled. Its minimum was 819.5 MiB overall and 1,273.5 MiB during the connection ramp. An estimate crediting half of clean file cache and zero reclaimable slab reached a minimum of 382.7 MiB. The largest interval between samples was 2.076 seconds, while Mojo checks at most once per second. These estimates do not reconstruct the physical guest's zone watermarks or memory outside the cgroup. [Memory replay](mojo-memavailable.json) · [lxcfs source](https://github.com/lxc/lxcfs/blob/lxcfs-5.0.4/src/proc_fuse.c#L1308) · [Linux MemAvailable calculation](https://github.com/torvalds/linux/blob/v6.8/mm/show_mem.c#L32)
