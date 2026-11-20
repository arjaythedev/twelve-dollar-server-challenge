# Reproduce the local HTTP comparison

The driver runs the challenge’s four application routes, one server and four client shards on separate CPUs of a Linux host. The client is Moneebullah’s native implementation of the browsing loop at challenge revision `3effe1d`. The included patch gives each shard a deterministic, disjoint user sequence, exports hold-only latency histograms and counts established sockets. The workload reads point-request IDs from the actual feed response, uses the signed tokens for writes, and observes the published think times and write probabilities. [Original client](../../../c-iouring-moneebullah25/tools/mload.c) · [Official k6 script](../../../../bench/load.js)

Build both servers with their own install and build scripts. From the challenge repository root, prepare scratch space on a disk-backed filesystem.

```sh
repo=$PWD
scripts=$repo/submissions/c-uring-willwang/tests/http
work=$(mktemp -d)
"$scripts/build-client.sh" "$work"
```

The following invocation requires Bubblewrap, permission to use private network namespaces, user systemd with delegated memory accounting, and CPUs 10 through 14. Choose available CPU numbers on your host. The benchmark harness applies the CPU and memory limits externally. The submitted server runs without affinity settings.

```sh
server="$repo/submissions/c-uring-willwang/bin/server"
users=311000
tag="candidate-$users"
bwrap --unshare-user --unshare-net --ro-bind / / --bind "$work" "$work" \
  --proc /proc --dev /dev --die-with-parent \
  python3 "$scripts/run.py" "$server" "$tag" \
    --work "$work" --source "$repo/seed/feed.db" --tokens "$repo/seed/tokens.json" \
    --cpu 10 --load-cpus 11,12,13,14 --memory-mib 2048 --vus "$users" \
    --ramp 60 --warm 90 --hold 300 --think 1 --port 19312
```

Repeat with `server="$repo/submissions/rust-iouring-kensac/target/release/feed-server"` and a new tag. All 15 tracked files in that submission at challenge revision `3effe1d` match the blobs in the scored #264 commit `4298101` (submission tree `94966f6cc58c436ee382107b937f3bfe02548966`). Build with its pinned Rust 1.94.0 toolchain. Each run starts on its own seed copy with a 65,535 server file-descriptor limit. The 60-second ramp is part of the 90-second warm-up. The result counts the following five minutes at full load. A smaller saturation diagnostic uses `--vus 4096 --think 0.004 --ramp 3 --warm 8 --hold 20`. [Scored Kensac source](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/4298101/submissions/rust-iouring-kensac)

Each `runs/TAG/result.json` records requests/s, merged client-side percentiles, request errors, TCP successes and failures, server CPU time and memory. `tcp.start` and `tcp.end` count simultaneous established sockets at the hold boundaries. Equal start, end, peak and handshake totals establish that every user kept its connection. The 2 GiB/no-swap limit encloses the initial database copy and the server. Cgroup peak memory includes charged sockets, slabs and page cache. The clients and host OS use additional memory outside that scope.

The native client waits for each response. `completed_at_or_above_60s` counts completed requests that reached the default k6 request timeout. After each hold the client also counts outstanding requests and records the oldest per endpoint. The recorded runs include the server, load-generator and seed hashes. [Measured results](results.json)

Run the head-to-heads consecutively on an otherwise idle host. With a negative `/proc/sys/net/ipv4/tcp_ehash_entries`, private network namespaces share the established-socket hash table. Concurrent socket tests can change lookup costs even with disjoint CPU sets. The runner records TCP allocation counts before ramp-up and at both hold boundaries. Per-shard `client*-seconds.csv` files give request counts and latency buckets for each second.

The recorded 311,000-user Kensac run and 315,000-user C run also had a host observer on CPU 2 reading per-thread I/O counters once per second. Those runs carry the additional `foreground_file_reads` field. The observer sampled procfs independently of the four clients.

The organizer uses a Synteq Xeon VPS and separate load generators in the same datacenter. This local comparison uses a native client over loopback. It scans feed JSON for IDs, puts the shard-local user number in each new post’s text and sends a smaller set of HTTP headers than k6. Its five-minute figures exclude ramp-up and ramp-down. Compare the implementations using the paired local measurements. [Official scoring environment](../../../../LEADERBOARD.md)
