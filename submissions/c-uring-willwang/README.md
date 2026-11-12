# A C feed server with AF_XDP and userspace TCP

This submission serves the challenge API from SQLite on one CPU. `start.sh` uses Linux TCP and io_uring. `start.sh --bypass` runs TCP in the server process, receives Ethernet frames through AF_XDP when XDP mode is selected, and can send batches through an AF_PACKET transmit ring. Both paths use the same HTTP handlers, authentication and database transactions. [Implementation and benchmark notes](IMPLEMENTATION.md)

## Local results

**342,500 concurrent users passed twice** with Linux TCP over loopback. The two five-minute holds recorded zero failures in 23,969,109 requests. All 350,000 and 400,000 sockets also opened in separate runs, where p99 rose to 1.362 s and 3.505 s. The organizer requires p95 below 500 ms, p99 below one second and fewer than 1% errors. [Loopback scores](tests/official/results.json) · [Challenge scoring rules](../../LEADERBOARD.md)

On private Ethernet, the unmodified Go generator used the standard 60-second ramp and 300-second hold. The fixed-ring build and Kensac’s pinned leaderboard release ran on the same link and CPU allocation.

| Ethernet server | Requested users | Successful requests/s | p95 | p99 | Errors | Verdict |
|---|---:|---:|---:|---:|---:|---|
| Fixed-ring AF_XDP | 250,000 | 24,563 | 72 ms | 111 ms | 0 | Pass |
| Fixed-ring AF_XDP, first run | 285,000 | 27,759 | 210 ms | 442 ms | 1 in 9,993,416 | Pass |
| Kensac, Rust 1.94.0 | 285,000 | 15,172 | 59.75 s | 59.75 s | 6.36% | Miss |
| Same fixed-ring executable, repeat after Kensac | 285,000 | 24,267 | 1.127 s | 59.75 s | 1.16% | Miss |

The 285,000-user pass occurred once in two attempts. In the repeat, the fixed-ring server delivered 59.9% more successful requests per second than Kensac and used 38.1% less server CPU per successful response. It answered a fresh database health check after disconnect. Kensac was OOM-killed during disconnect. In a separate 32,000-connection comparison against this submission’s Linux TCP path, bypass reduced kernel CPU time per request by 26.6% and total server CPU by 18.2%. The local virtual Ethernet driver used AF_XDP copy mode. [Ethernet scores and CPU accounting](tests/bypass/results.json) · [Reproduction commands](tests/bypass/README.md)

## What does the work

- **Find the feed with one rank check.** The reader fetches the last 20 posts by rowid and checks the ID at position 20 in the live chronological index. When the worst candidate matches that boundary, the candidate set is the feed. When the proof fails, the reader uses the general chronological plan. Compiling the reader with SQLite’s C amalgamation lets Clang inline the B-tree operations. [Feed reader](reader.c) · [Proof and comparisons](IMPLEMENTATION.md#feed-and-point-reads)
- **Index TCP connections and skip idle peers in the fast timer.** Two process-keyed hash tables index active and `TIME_WAIT` connections. The 250 ms timer tracks the oldest connection that needs work, skipping idle peers outside that prefix. lwIP 2.2.1 supplies retransmission, congestion control and receive windows. The fixed 512-slot sender groups compatible TCP segments for segmentation offload. [Flow index](bypass/lwip-pcb-index.patch) · [Timer](bypass/fast-timer.md) · [Segmentation](bypass/packet_gso.h)
- **Batch database and socket work.** The Linux transport gives each sparse io_uring file table its own epoll readiness queue. Both transports batch SQLite writes, release successful write responses after commit, and run WAL checkpoints on a second connection. [Linux reactor](reactor.c) · [Transaction handling](database.c) · [WAL implementation](wal.c)

The optional 40 ms ACK deadline and retry correction eliminated 2,993 client retransmissions in a paired test of fragmented requests. That configuration missed the full 285,000-user browsing thresholds. The measured default uses the 250 ms timer. [TCP experiment and wire tests](bypass/ack-deadline.md)

## Build and run

From the challenge checkout on Ubuntu 24.04, install the dependencies as root and build as the deploy user. For a local smoke test, give the server a writable copy of the seed.

```sh
entry=submissions/c-uring-willwang
sudo bash "$entry/install.sh"
bash "$entry/build.sh"
test -f seed/feed.db || bash seed/make-seed.sh
run_dir=$(mktemp -d)
cp seed/feed.db "$run_dir/feed.db"
HOST=127.0.0.1 PORT=3000 SQLITE_PATH="$run_dir/feed.db" \
  JWT_SECRET=twelve-dollar-challenge "$entry/start.sh"
```

From another terminal, `curl -fsS http://127.0.0.1:3000/health` checks the database through the running server. [Runtime contract](../../SPEC.md#runtime-contract)

For single-IP AF_XDP on a self-managed VM, the installer can configure an existing systemd service. Substitute its unit, interface, service address and gateway below. The unit supplies the runtime environment. The installer grants startup capabilities, and the server drops them after opening its packet rings.

```sh
sudo bash submissions/c-uring-willwang/install.sh --bypass-unit feed.service ens3 192.0.2.22/24 192.0.2.1
sudo systemctl restart feed.service
```

The published challenge unit grants `CAP_NET_BIND_SERVICE`. The organizer has held a request for the additional BPF and network-admin capabilities pending an eligibility decision. The ordinary Linux TCP launch works with the published permissions. [Installer and unit details](IMPLEMENTATION.md#installing-packet-access) · [Organizer’s response](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/231#issuecomment-6547094174)

The Ethernet build passed the organizer’s 42 checks over the wire and all 42 again over localhost. Additional tests cover 13 database layouts, TCP retransmission and half-close, and a blocked WAL commit followed by `ENOSPC` and crash recovery. [Correctness and resource tests](tests/bypass/README.md)
