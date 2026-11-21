# Single-queue TAP benchmark with a dedicated service reader

Four stock Go load generators send real Ethernet frames through a private switch to one Linux TCP server. The service has one 1,500-byte TAP, one RX queue, one TX queue and one threaded NAPI task pinned to the same CPU as the application. The switch adds 75 µs in each direction. Giving its service TAP reader a dedicated external core removed the transmit-ring loss observed when that reader also delivered packets to four Linux client stacks. The application and scorer both run as the calling user. Root privileges are used by the local fixture to create temporary network namespaces, configure their devices and establish the transient service cgroup. [Shared fixture setup](../router/README.md) · [Original single-queue TAP and CPU placement trace](../tap-poll/README.md)

## Run

Start from the challenge checkout on Linux with a C compiler, systemd, cgroup v2, iproute2, ethtool, sudo and a working NAPI-capable `/dev/net/tun`. The common setup supplies the stock mload executable and the seeded database. Set `work` and `entry` to absolute paths. The commands below build only the local switch.

```sh
fixture="$PWD/submissions/c-uring-willwang/tests/official/tap-split"
work=/absolute/path/to/benchmark-work
entry=/absolute/path/to/submission
mkdir -p "$work"
export TWELVE_SPLIT_TAP_RELAY="$work/split-tap-relay"
cc -O3 -g -Wall -Wextra -Werror -std=gnu11 -pthread \
    "$fixture/relay.c" -o "$TWELVE_SPLIT_TAP_RELAY"

python3 "$fixture/run_split.py" --wire-proof-only --output "$work/split-wire-proof"

python3 "$fixture/run_split.py" --server "$entry/bin/server" --cwd "$entry" \
    --mload "$work/mload" --seed "$PWD/seed/feed.db" --tokens "$PWD/seed/tokens.json" \
    --users 300000 --hold 300 --output "$work/runs" --tag candidate-split-300k
```

The proof and scored trial accept `--app-cpu`, `--router-cpus`, `--client-cpus`, `--manager-cpu` and `--delay-us`. Defaults reserve CPU 10 for the service and its NAPI task, CPUs 2, 5, 8 and 14 for the switch, CPUs `0,1;3,4;6,7;11,12,13` for the four generators and CPU 15 for sampling. Choose distinct physical cores for these roles and hold the assignments constant across submissions. The runner serializes trials, makes a fresh copy of the seed, and verifies the application service's 1,750 MiB memory limit and 65,535 FD limit. It also repeats the route and payload proof before each scored trial. [Limits, scorer build and preserved artifacts](../router/README.md)

## Wire behavior and measurements

The final external core in `--router-cpus` continuously reads the single service TAP. Four bounded single-producer/single-consumer FIFOs carry service replies to their destination workers. With the default affinities, CPU 2 reads requests from generators 1 and 4 and delivers replies to generator 1. CPU 5 reads generator 2 and delivers replies to generators 2 and 4. CPU 8 reads and delivers generator 3. The reader preserves single-lane packet order, shares the Ethernet payload and virtio header across broadcast recipients, and reuses each frame only after every addressed recipient releases it. Each destination holds up to 2,048 frames. The native service TAP transmit ring remains at its observed 1,000 entries. [C tests and binary verification](../tap-sharded/VALIDATION.md)

The five-interface preflight verified 345,184 response bytes across four TCP connections and 128 exchanges, MSS 1,448, 16 independent socket CPU checks, and all 80 ICMP probes. Lane median RTTs were 0.262–0.277 ms. One probe on the fourth lane took 4.57 ms. The switch forwarded 52 GSO frames and 418 frames carrying partial-checksum metadata. Its four destination queues delivered every admitted frame. The captured interface, protocol, CPU and cleanup evidence is available in the recorded proof. [Four-lane route proof](wire-proof.json)

`network-samples.jsonl` preserves service and generator interface counters, the service qdisc, relay worker activity, per-lane FIFO depths and delivery-lateness histograms. `summary.json` separates the ramp and hold and reports each worker's least idle interval. Empty polls are cheap, so use idle wall time alongside idle scan count. `relay_worker_scheduling` records each private relay worker's runtime, runnable wait and context switches when its kernel exposes them. Producer and consumer counters publish independently every 50 ms. Use the sampled atomic `queue_depth` for instantaneous occupancy and the post-join counters in `tap-relay-current.json` for final reconciliation.

A controlled test sent 1,100 IPv4 frames to an unread 1,000-entry private TAP. It recorded 100 interface TX drops, zero qdisc drops and precisely 100 `tun_net_xmit` frees with reason `FULL_RING`. A service TAP TX drop with a clean qdisc therefore warrants investigation of the external reader. A service TAP RX drop is recorded separately as a target-side signal. [Driver control](wire-proof.json) · [Linux 6.8 TUN transmit path](https://github.com/torvalds/linux/blob/v6.8/drivers/net/tun.c#L1133)

## Read the score

Two trials shared the four external switch cores with a relay left running after its launcher was interrupted. At 280,000 users Pavlyhalim’s four switch workers each spent 56.3–59.1% of their hold time waiting to run. The generator TAPs lost 727,181 transmit frames and the service TAP lost 337,787. The 300,000-user C trial also overlapped this relay. Stopping it reduced all four workers’ runnable wait from 51.25–51.49% to 0.00–0.12% in the next eight-second sample. The assessment flags both trials. Run `python3 relay_schedule.py RUN_DIRECTORY` in this directory to inspect the scheduler counters stored by a completed or active fixture. [Scheduler measurements](relay-lifecycle-proof.json)

The reproduction relay watches the launcher's control pipe from its once-per-second reporting thread. Across four private tests that killed the launcher before or after it started polling, the relay exited in 18–26 ms. The archived capacity trials used the same frozen switch executable throughout. [Build identities and lifecycle tests](relay-lifecycle-proof.json) · [Build commands](../tap-sharded/VALIDATION.md)

`stock_pass` is the unmodified mload decision from the merged full ramp-and-hold histograms. `scored_healthy_environment` additionally describes whether all four shard durations completed, the full user population was scheduled throughout the sampled hold, the service avoided OOM, and the external network and generators passed their health checks. `qualified` requires a stock pass, a healthy external path, clear target receive counters, and the full client connection population at **every** expected five-second sample.

Each Go shard increments `users` when it schedules a virtual user. That user waits a random 0–5 seconds before its first request. The shard increments `conns` after a successful TCP dial and decrements it when mload closes that connection. The harness aligns all four stock timelines to their shared launch time and sums the same five-second ticks, beginning at least five seconds after the ramp. Each timestamp allows one second of reporting jitter. The intervals between ticks remain unobserved. [Stock Go counter definitions](../mload/main.go)

The independent service-side `Tcp.CurrEstab` samples count kernel sockets in ESTABLISHED or CLOSE_WAIT in the service namespace. They run on a different sampling clock and expose a single aggregate. For example, one 300,000-user trial had all 300,000 users at all 59 stock ticks. The generators counted 299,999 live connections on the first expected tick at 95 seconds and 300,000 at all 58 subsequent ticks. Every sampled service-side count was 300,000. The aggregate passed the stock scorer and the external-health checks, while the strict local connection indicator remained false.

## Interpret application CPU

The organizer publishes application cgroup CPU during the hold. On the local Ubuntu 6.8 kernel, ordinary receive softirqs can run while the service is the scheduled task. Linux records those ticks as CPU softirq time, and this kernel's scheduler can also charge that interval to the service cgroup. A dedicated NAPI worker instead gets its own scheduler runtime. In two local 280,000-user holds of the same original binary, ordinary RPS reported 97.97% application cgroup CPU together with 60.53% CPU softirq. Threaded veth reported 52.90% and 12.29% respectively. The application core was at least 99.9% busy in both. Each run had external network flags, so those figures describe accounting and cannot qualify a capacity score. [Measured windows and kernel configuration](cpu-accounting.json) · [Linux task clock](https://github.com/torvalds/linux/blob/v6.8/kernel/sched/core.c#L695) · [Linux task and cgroup accounting](https://github.com/torvalds/linux/blob/v6.8/kernel/sched/fair.c#L1106)

In the clean split 300,000-user trial, the hold alone measured 48.62% application cgroup CPU and 48.00% private NAPI runtime across 298.31 seconds of telemetry. The NAPI worker averaged 8,370 scheduler slices per second, 57.35 µs of runtime and 8.35 incoming TCP segments per slice. Across the 388.98 sampled seconds of ramp plus hold, the worker used 45.18% and ran 3,483,719 times. These times include actual GRO and TCP processing. The count of slices alone cannot estimate the processor time lost to scheduling. `host_checks.cpu_accounting_windows` preserves the actual hold, ramp and combined sample boundaries for the application, core and NAPI. `shape.json` records the relay ELF digest and the launch-time digests of the Python setup, observation and scoring helpers. Older records preserve the counters available when their runner started.

A separate four-trial control changed only the receive mode of a fresh private TAP pair. Both modes returned identical bytes and reported the service socket's receive CPU as 9. With a threaded service TAP, all 184 traced device polls in each trial ran on CPU 9. With ordinary TAP NAPI and RPS steering the service TCP toward CPU 9, all 187 and 185 device polls ran on the external injector's CPU 15. Socket CPU alone therefore misses the cost of device polling and GRO. The remote-RX, 82%-quota sensitivity model can rank the same binaries under a fixed app budget, but it moves some work outside the application CPU. The organizer's 82% also includes any inline interrupt work that its own guest kernel charged to the cgroup. Its published scores range from 81% to 96% across the top five. Converting one published percentage into a fixed execution budget requires knowing that kernel's accounting and actual NIC interrupt behavior. [Private ABBA traces](cpu-accounting.json) · [Linux NAPI scheduling](https://github.com/torvalds/linux/blob/v6.8/net/core/dev.c#L4444) · [Official scores and metric definitions](https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/8b20e65/LEADERBOARD.md)
