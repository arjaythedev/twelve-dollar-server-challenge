# Comparing a userspace TCP stack with Linux TCP

The lab runs the submission's SQLite/HTTP service through the organizer's stock Go client. A single 1,500-byte MTU veth joins two disposable Linux namespaces. Set the server CPU, client CPUs and observer CPU to separate physical cores on your machine. Receiver packet steering puts each endpoint's work on its own CPUs.

## Build and run

Use Linux with a local rootful Docker daemon, CPU accounting and memory delegation through systemd user scopes, Python 3.11 or newer, Bash, curl, Go 1.27.1 or newer, and a C compiler with static libc development files. Go 1.27.3 reproduces the recorded linux/amd64 load client. Set `GO_BIN` to select an installed compiler. The packet run uses a nonroot host account with access to its local Docker socket. The scored application receives an already-open descriptor for the private interface. The dedicated link has no host-side interface, Internet uplink or forwarding. The XDP mode uses a separate bootstrap inside the isolated server namespace.

From the submission directory, build the ordinary server and the bypass server with their documented commands. Build the local stock client and fixture. Specify the challenge's seeded SQLite database and tokens. Each output directory must be new.

```sh
work=/tmp/twelve-bypass-tools
seed=/absolute/path/to/feed.db
tokens=/absolute/path/to/tokens.json
sh tests/bypass/build-lab.sh "$work"
python3 tests/bypass/lab.py probe --work "$work" --tokens "$tokens" --out /tmp/twelve-link
python3 tests/bypass/lab.py golden --mode packet --binary bin/server-bypass \
    --work "$work" --seed "$seed" --tokens "$tokens" --out /tmp/twelve-packet-golden
python3 tests/bypass/lab.py differential --mode packet --binary bin/server-bypass \
    --work "$work" --tokens "$tokens" --out /tmp/twelve-packet-oracles
python3 tests/bypass/lab.py score --mode packet --binary bin/server-bypass \
    --work "$work" --seed "$seed" --tokens "$tokens" --out /tmp/twelve-packet-score \
    --server-cpu 10 --client-cpus 3,4,5 --manager-cpu 15 \
    --users 6000 --ramp 8 --hold 32 --think .05
```

Repeat the same command with `--mode baseline --binary bin/server` and a fresh output location to compare Linux TCP. `--mode xdp --binary bin/server-bypass` selects the dedicated AF_XDP path on setups that support it. For a three-way comparison pass `--wire-checksums` to **all** arms. This private client-veth setting makes Linux finish the wire checksum on transmit and saves the resulting feature masks in each output directory. The suite verifies the API against the challenge's 42 checks and compares 13 independent SQLite database layouts against reference SQL. Its transport checks include fragmented/chunked requests, large bodies, backpressure, half-closes, resets and, on the userspace stack, a TCP retransmission observed by its ordinary Linux peer.

For a one-server-core AF_XDP comparison on the direct veth, also pass `--xdp-attachment skb --rx-cpu-stats`. Generic XDP executes after receive packet steering. The lab records the BPF redirects per actual CPU and verifies they all executed on the chosen server CPU. Native driver XDP on a direct veth executes the redirect on the transmitting client's CPU. Both attachment modes use AF_XDP copy on this device. `--packet-rx-frames` changes only the AF_PACKET receiver ring capacity for separate burst/teardown experiments.

To test deployment on a single IP, run `golden --mode xdp --shared-ip --xdp-attachment skb --wire-checksums` with the same binary, seed, tokens and a fresh output directory. The lab runs the unchanged organizer script once from a private Ethernet peer, then again on a fresh database from inside the server namespace using `127.0.0.1`. It checks that both transports see each other's writes, a fragmented chunked POST and a half-closed pipeline. It also opens the database independently after stopping the server and checks the acknowledged writes. The two organizer logs, server-side Linux TCP segment deltas and recovered row counts go in the run directory. The localhost checker also requires GNU coreutils, jq and OpenSSL on the lab host. [Local and remote checks](local_http.py)

The hybrid resource test holds an actual SQLite WAL write before checking that the client has received no response. It then returns `ENOSPC`, verifies the HTTP error, kills the service and checks the surviving committed row through an independent SQLite connection. The same test covers bounded admission, a slow reader, pipelined half-close and a retransmission observed by the Linux client.

```sh
python3 bypass/tests/resources.py --xdp-hybrid --work "$work" \
    --out /tmp/twelve-hybrid-resources --binary bin/server-bypass \
    --server-cpu 9 --client-cpu 14 --manager-cpu 15
```

### Native driver XDP on the server CPU

Add `--native-bridge --wire-checksums` to each arm of a matched comparison. This topology inserts a third disposable network namespace containing a two-port Linux bridge with no IPv4 address. The client-facing bridge port steers incoming frames to the server CPU before forwarding them to the server veth. The reverse bridge port also uses the server CPU. The final client veth steers replies to the client CPUs. The measured server core therefore includes extra Linux bridge forwarding in both directions. The host network has no interface on this link.

With `--mode xdp --native-bridge` and the default attachment, the scorer verifies the actual driver attachment and checks the CPU of every observed XDP redirect. A four-connection probe on this machine placed all four native redirects on server CPU 12, and the real Linux client's `SO_INCOMING_CPU` reported CPU 13 for all four TCP RST replies. The packaged runner checks four HTTP health replies before the stock workload starts and saves their observed receive CPUs in `bridge-reply-placement.json`. The run directory also records feature masks, bridge ports, kernel network namespace identities and per-device RPS masks. The bridge adds an 8 MiB helper. The runner caps the combined application, broker and endpoint limits at 1,750 MiB. On the default settings the Linux and AF_PACKET applications each receive 1,722 MiB and the XDP application receives 1,730 MiB. This option requires local iproute2 and an already loaded Linux bridge driver.

## Load and accounting

Paired tests measured immutable response buffers, established TCP header prediction and cumulative ACK retirement. The experiment record includes packet and state checks, transport CPU at 350,000 resident connections, and HTTP comparisons with real Linux peers. [TCP experiments](tcp-experiments.md)

`--xdp-rx-frames` configures RX descriptors and the FILL ring. `--xdp-tx-frames` configures TX descriptors and the completion ring. At this MTU the 8,192/2,048 configuration uses 20 MiB of UMEM inside the service cgroup. For large runs set `--close-timeout 300` to observe late Linux FIN retries and subsequent TCP keepalives. The lab keeps the original process and client network namespace alive throughout observation. It verifies service health, committed data and the remaining connection count independently.

The default launcher `start.sh --bypass` selects `bin/server-bypass-fixed`, which has a 512-slot AF_PACKET transmit ring. Each slot holds 8,192 bytes, for a 4 MiB ring. To set a different capacity on AF_PACKET or hybrid `--mode xdp --packet-gso`, supply `--packet-tx-frames N` to `start.sh` or select the configurable `--binary bin/server-bypass` explicitly in this lab. A 4,096-slot ring uses 32 MiB from the same application budget.

For the organizer's full schedule use `--ramp 60 --hold 300 --think 1`. The shorter example exercises more requests per connection by scaling think time. All virtual users create ordinary TCP sockets. Multiple stock Go shards select separate real Linux source IPs through private per-UID routing. Each shard receives disjoint virtual-user offsets, an independent output directory and the same start clock. For 350,000 or 400,000 VUs use 32 source addresses (`--shards 32`) and enough client CPUs and RAM. The runner records the verified private ephemeral port range, per-shard addresses, binary and seed hashes, resource limits and actual simultaneous populations.

The recorded high-population hybrid uses generic AF_XDP receive and AF_PACKET transmit with TCP segmentation offload. On the 16-core test machine the complete invocation was

```sh
python3 tests/bypass/lab.py score --mode xdp --binary bin/server-bypass-fixed \
    --xdp-attachment skb --rx-cpu-stats --xdp-rx-frames 8192 --packet-gso --wire-checksums \
    --work "$work" --seed "$seed" --tokens "$tokens" --out /tmp/twelve-hybrid-350k \
    --server-cpu 10 --client-cpus 0,1,2,3,4,5,6,7,8,11,12,13 --manager-cpu 15 \
    --users 350000 --shards 32 --ramp 60 --hold 300 --think 1 --close-timeout 300
```

For the matched Linux run use `--mode baseline --binary bin/server --wire-checksums` with the same workload, CPU settings and a fresh output directory. `scored-summary.json` preserves whole-core and packet counts at cutoff. `post-close.json` records the original process, peer counts and health result before container disposal. The independent SQLite check runs on the original database after the process stops. The organizer's request rate includes failed attempts. Successful requests per second and the largest simultaneous Linux client population are reported separately.

To reproduce the 285,000-user comparison, change the count to `--users 285000` and add `--shared-ip` on the userspace arms. Run the configurable `bin/server-bypass` with `--packet-tx-frames 512`, then the rank-one server with `--mode baseline --binary /path/to/feed-server --wire-checksums`, then the default `bin/server-bypass-fixed` with its built-in 512-slot ring. Give each run its own output directory and wait for its closure report. The measured rank-one executable came from commit `6d6ec94` with Rust 1.94.0, the checked-in Cargo lockfile and `RUSTFLAGS='-C target-cpu=native'`. Its SHA-256 is `b5cf9b042a4d1237e97438f863e72992ff4d81de01312bbc4249d0f48a2e1557`. [Kensac source](../../../rust-iouring-kensac)

Read the organizer's `pass` field in `score.json` to determine whether the latency and error thresholds passed. The runner's exit status reports whether the experiment and its integrity and disconnect checks completed successfully. This also allows a deliberate overload run to produce a complete set of measurements.

For a long run launched from a transient shell, run the Python command as a supervised user service. Prefix it with `systemd-run --user --unit=NAME --working-directory="$PWD" --property=StandardOutput=append:/absolute/path/driver.log --property=StandardError=inherit`. A fresh unit name and log path identify each independent run.

Linux `Tcp.CurrEstab` counts sockets in `ESTABLISHED` and `CLOSE_WAIT`. The lab samples it once per second and records the stock generators' own connected count on five-second ticks. Each generator can dial again after a transport failure, so dial totals can exceed both the configured users and the peak simultaneous population.

The ordinary server uses Linux's address `10.241.1.1`. The userspace service owns `10.241.1.253` on the same private interface. Its namespace has IP forwarding off and no Linux address for that service. The packet descriptor broker opens only the dedicated private veth, then drops its capabilities. The local applications run with hard/soft fd limit 65,535. The default AF_PACKET resource budget combines a 1,730 MiB application scope, 8 MiB server endpoint and 12 MiB descriptor broker, all with zero swap. The baseline receives the same allocation. The XDP bootstrap uses the application cgroup itself and the 8 MiB endpoint.

Each `go*/score.json` contains the untouched organizer result. `score.json` combines its original histograms using the organizer's merge program. `summary.json` reports the scored ramp-and-hold result, observed Linux peer population, whole physical server CPU including softirq/IRQ, application cgroup CPU, and an interval bounded by complete five-second ticks in which every stock client was connected. These CPU measurements overlap. The full-core measure also includes unrelated work scheduled on that core; an otherwise idle machine and matched repeats matter. Sampled memory includes socket buffers and page-cache charges. The runner compares the durable increase in SQLite post rows with the number of successful stock create responses, and runs `PRAGMA quick_check`. The separate differential and WAL tests recover specific acknowledged post IDs after a process crash. `samples.jsonl`, `closure.jsonl`, `after-close.jsonl` and the original server stderr retain raw packet, CPU and disconnect evidence. A userspace run verifies that Linux server TCP received and transmitted zero TCP segments while Linux clients had live connections.

## Measurements

The 32,000-user test used three source addresses, 10 seconds of ramp, 30 seconds of hold and a think scale of 0.16. All arms returned zero errors and delivered about 21,100 responses/s in the common all-connected interval from second 15 to 35. Microseconds per successful response come from physical CPU counters. The combined column includes the complete stock Go clients. [Measurements and experiment settings](results.json)

| Transport | Server CPU (µs) | Server + clients (µs) | Ingress work location |
| --- | ---: | ---: | --- |
| Linux TCP | 36.81 | 159.24 | Server CPU |
| AF_PACKET | 34.93 | 167.96 | Server CPU |
| Generic AF_XDP | 31.51 | 171.00 | Server CPU |
| Generic AF_XDP + AF_PACKET/GSO transmit | 30.11 | 140.49 | Server CPU |
| Driver AF_XDP + AF_PACKET/GSO transmit, private bridge | 32.32 | 149.46 | Server CPU, including both bridge directions |
| Driver AF_XDP, direct veth | 22.04 | 148.16 | Redirect and UMEM copy on the transmitting client CPU |

The high-load trials used the organizer's full 60-second ramp, five-minute hold and normal think time. These results include the entire ramp. All rows used the same direct veth, server CPU 10, a 1,730 MiB application limit with zero swap, and 32 stock generators on 12 other CPUs. [Complete scores, binary hashes and settings](results.json)

| Transport | Configured users | Peak Linux client sockets | Successful req/s | Errors | p95 | p99 | Original process after disconnect |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Generic AF_XDP + AF_PACKET/GSO, shared service IP, `61fcf2d5` | 250,000 | 250,000 | 24,563 | 0% | 72.34 ms | 110.97 ms | Healthy and empty when checked at +296.9 s |
| Generic AF_XDP + AF_PACKET/GSO, shared service IP, `61fcf2d5` | 285,000 | 285,000 | 27,759 | 0.00001% | 209.78 ms | 442.46 ms | Healthy and empty at +90.9 s |
| Configurable generic AF_XDP + AF_PACKET/GSO, explicit 512 TX slots, shared IP, `459ed2e7` | 285,000 | 284,857 | 25,858 | 0.500% | 844.82 ms | 13.701 s | Healthy and empty at +195.7 s |
| Kensac rank-one Rust release, Linux TCP, `b5cf9b04` | 285,000 | 285,000 | 15,172 | 6.358% | 59.746 s | 59.746 s | OOM kill at cutoff +2.59 s |
| Rebuilt fixed-512 generic AF_XDP + AF_PACKET/GSO default, shared IP, `61fcf2d5` | 285,000 | 284,657 | 24,267 | 1.159% | 1.127 s | 59.746 s | Healthy and empty at +195.8 s |
| Generic AF_XDP + AF_PACKET/GSO, wider flow index, 40 ms ACK deadline, 4,096 TX slots, shared IP, `9b07fbc9` | 285,000 | 284,961 | 24,703 | 0.701% | 1.031 s | 35.612 s | Healthy and empty at +190.1 s |
| Generic AF_XDP + AF_PACKET/GSO, 40 ms ACK and transmit retry, original 17-bit table and 512 TX slots, shared IP, `2b402ac5` | 285,000 | 284,478 | 25,350 | 0.768% | 933.21 ms | 19.799 s | Healthy and empty at +148.3 s |
| Canonical generic AF_XDP + AF_PACKET/GSO, 512 TX slots, shared IP, `459ed2e7` | 300,000 | 299,322 | 25,502 | 1.222% | 1.031 s | 59.746 s | Healthy and empty at +156.4 s |
| Canonical generic AF_XDP + AF_PACKET/GSO, 512 TX slots, shared IP, `459ed2e7` | 310,000 | 307,213 | 25,323 | 1.814% | 1.296 s | 59.746 s | Healthy and empty at +200.6 s |
| Linux TCP | 350,000 | 350,000 | 20,819 | 0.00093% | 8.498 s | 11.684 s | OOM kill at cutoff +1.86 s |
| Generic AF_XDP + AF_PACKET/GSO, `ddf68d7c` | 350,000 | 341,046 | 23,868 | 4.646% | 6.368 s | 59.746 s | Healthy, all accepted connections released by +185.6 s |
| Generic AF_XDP + AF_PACKET/GSO, custom TCP deadline calendar, `e2839141` | 350,000 | 342,576 | 22,235 | 5.697% | 20.197 s | 59.746 s | Alive with zero accepted connections at +194.6 s |
| Generic AF_XDP + AF_PACKET/GSO, `61fcf2d5` | 400,000 | 387,135 | 20,888 | 9.566% | 59.746 s | 59.746 s | Healthy, all accepted connections released by +187.1 s |

All three 350,000-user runs shown in the table missed the organizer's limits. Linux reached the entire requested population and returned fewer errors. The hybrid server delivered 14.6% more successful requests per second and survived the synchronized close. SQLite `quick_check` passed for the shared-IP runs, the Linux OOM and the two regular hybrid target runs. Its server journal recorded the kill while 348,640 native sockets were still established or in `CLOSE_WAIT`. The release binaries and all measured packet paths are local lab results.

The custom TCP calendar trial isolated its scheduler against the same 512-slot transmit ring. Despite quicker timer handling in the packet fixture, it completed 6.8% fewer successful HTTP requests per second than the regular hybrid at 350,000 users. All 30,562,906 redirects were recorded on server CPU 10. The process peaked at 690,774,016 bytes with zero OOM kills. The Docker API timed out during disposal after the application exited cleanly. Its complete score and final empty-connection samples survived, but the health response verdict and independent database check are unavailable for this trial.

The `61fcf2d5` 285,000-user shared-IP hold passed all three stock thresholds. The original process peaked at 427,986,944 bytes with zero OOM kills. SQLite retained 90,181 new posts against 90,110 acknowledged creates, and the inclusive physical core used 34.89 µs per successful response. The single timeout occurred among 9,993,416 requests.

The packaged configurable build, `459ed2e7`, ran this same 285,000-user protocol immediately before the pinned rank-one comparison. It peaked at 284,857 established client sockets, delivered 9,309,201 successful responses, and timed out on 46,746 requests. Its p95 was 844.82 ms and its p99 was 13.701 s. The full paid core used 37.57 µs per successful response, including 198.98 s of softirq work. This build used the same fast TCP timer and 17-bit flow table as `61fcf2d5`, with the 512-slot transmit capacity set explicitly.

Kensac reached 285,000 simultaneous connections on both endpoints by second 67. Its 5,833,160 attempts included 370,873 timeouts. It completed 15,172 successful requests/s and used 64.51 µs of paid CPU per successful response. The immediately preceding configurable build delivered 70.4% more successful requests/s and used 41.8% less paid CPU per success. Both missed the latency thresholds. During disconnect, Kensac's cgroup recorded an OOM kill 2.59 s after cutoff, with 281,731 native sockets still established or in `CLOSE_WAIT`. All sockets had drained by +29.2 s. After the original process died, independent SQLite `quick_check` passed and the database contained 48,882 new posts against 46,820 acknowledged creates.

The fixed-512 default ran again immediately after Kensac, using the same `61fcf2d5` executable, seed, 32 source allocations, CPU placement and ring settings as its first 285,000-user pass. It completed 24,267 successful requests/s, with 102,471 failures in 8,838,891 attempts. That was 59.9% more successful traffic than Kensac at 39.93 µs of paid CPU per success, a 38.1% reduction. Its p95 was 1.127 s. Both servers' p99 reached the 59.746 s histogram bucket. The default's original process survived, used a peak of 536,162,304 bytes and answered `/health` after all connections drained. SQLite retained 78,582 new posts against 78,075 acknowledged creates and passed `quick_check`. The fixed default passed one of its two independent 285,000-user trials. The 250,000-user result is a separate passing full hold.

At the same 285,000 configured users, changing the flow-table width, delayed-ACK deadline and transmit capacity together produced 62,792 failed attempts and increased paid CPU from 34.89 to 39.35 µs per successful response. Linux clients recorded 4,107,492 retransmissions versus 451,337 with the canonical release. This comparison measures the three changes together. A second treatment kept the original 17-bit table and 512 TX slots, with a 40 ms delayed-ACK deadline and a retry after a refused ACK transmission. It eliminated all 2,993 peer retransmissions in a matched 2,048-peer partial-request fixture. In a full 285,000-user stock run it still missed both latency cutoffs and had 70,635 timeouts. The canonical 300,000- and 310,000-user trials used the later packaged `459ed2e7` binary and explicitly requested its 512-slot transmit ring. Both retained the canonical 17-bit flow table and fast TCP timer.

The 250,000-user hold also passed all three stock thresholds. An executor restart interrupted the independent resource sampler at second 261 of 360. All 32 load generators continued and saved their complete original histograms and zero exit codes. On reattachment, the same server process answered `/health`, with zero client sockets, accepted application connections and buffered bytes. Its cgroup recorded a 378,060,800-byte lifetime peak and zero OOM kills. The recorded 296.9 seconds is an upper bound on time to drain. Whole-run CPU is unavailable for this run. The independent database check found 79,374 new post rows against 79,360 confirmed creation responses.
