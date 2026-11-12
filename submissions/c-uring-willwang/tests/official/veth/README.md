# Run the stock Go workload over private Linux veth links

This harness puts the submitted server and the organizer’s Go clients on one Linux host, with a separate kernel receive CPU for each side. It creates direct veth pairs between disposable network namespaces. Both endpoints use ordinary kernel TCP, including connection setup, ACKs and FINs. The server performs its own bind, listen, accept, HTTP, io_uring and SQLite operations.

The measured full layout has four Go processes, each connected to two private server namespaces. Each of the eight original listening sockets accepts connections on twelve IPv4 addresses in its namespace. Each client has one source address per link. With the standard 32,768–60,999 port range, this supplies 2,710,272 available source/destination tuples. The stock scorers have a common start time and disjoint VU offsets. The local distribution across listening sockets follows the chosen destination address. A comparison with another implementation needs the same listener layout. The organizer’s separate load-generator machines have different network latency, listener distribution, hardware and kernel settings.

## Host requirements

- Use Linux with veth, IPv4, receive packet steering, a readable `/proc/sys/net/netfilter/nf_conntrack_count`, cgroup v2 CPU and memory accounting, swap controls, and a working `systemd --user` session. The host and application must support the server’s io_uring features. The tested host ran Linux 6.8.
- Run as an ordinary user authorized to use the local rootful Docker Engine at `/var/run/docker.sock`. The daemon must support `--network none`, read-only containers, CPU and memory limits, and the per-network-namespace `net.core.rps_default_mask` sysctl. Docker access grants control of the daemon. The running native application must have a nonzero UID, zero effective and permitted capabilities, and a hard fd limit of at least 65,535 before launch.
- Install Python 3.11+, a C compiler with static libc and Linux headers, Go for the offline stock client build, and the standard Linux `taskset` and systemd tools. Use a disk-backed work directory large enough for the seed copy and SQLite WAL. The host also needs memory for both sides of the test. Each stock Go container has an 8 GiB limit and a 262,144 fd limit.
- Use a dynamically linked native server which obtains its IPv4 TCP sockets through libc. The startup interposer obtains unbound sockets from each private namespace in round-robin order. This submission opens eight listeners on `0.0.0.0`; the examples allocate all eight. A static binary, a server issuing the socket syscall directly, or a different listener layout requires a compatible socket-allocation harness.
- Choose CPU numbers from `lscpu -e=CPU,CORE,SOCKET` and the caller’s allowed CPU set. Reserve a physical core for the server, keep stock client groups disjoint and use an independent management CPU for full measurements. The two-CPU smoke below shares management with the client.

Docker creates each private namespace with only loopback. Two short-lived helpers receive only `CAP_NET_ADMIN` in those disposable namespaces, check the supplied peer network namespace, create and configure the veth, clear capabilities and exit. The load generator and persistent socket providers run as UID 65532 with all capabilities dropped. Their containers retain Docker’s default security profiles and use read-only root filesystems. The driver verifies the actual private RX-queue masks and the original host RPS default. It does not configure host interfaces, firewall rules, routing, sysctls or global fd limits.

The native server runs outside Docker in a user scope capped at 1,750 MiB, with zero swap and a 65,535 soft and hard fd limit. The eight permanent socket providers each have an 8 MiB limit on the server CPU. The reports record their separate CPU and memory costs. Kernel allocations in softirq context can fall outside the application memory cgroup. Per-CPU measurements include softirq work and anything else scheduled on those CPUs.

## Build

Start in this directory. Build the server using its submission instructions. Generate the challenge’s seed and tokens with `seed/make-seed.sh` if needed, then choose an empty disk-backed work directory. Use the pristine, checkpointed fixture. Only the main seed file is copied. All binaries, temporary database copies and measurement output go outside this directory.

```sh
challenge=$(cd ../../../../.. && pwd)
work=$(mktemp -d /var/tmp/twelve-veth.XXXXXX)
server="$challenge/submissions/c-uring-willwang/bin/server"
seed="$challenge/seed/feed.db"
tokens="$challenge/seed/tokens.json"
test -r "$seed" && test -r "$tokens" && test -x "$server"

../build-client.sh "$work"
./build.sh "$work"
```

The parent build script checks the upstream source hashes, builds offline and records the Go binary hash and toolchain. To select a specific Go executable, set `GO_BIN` before running it. Go 1.27.3 on Linux amd64 produces the scored client SHA-256 `48df1cd6242f55ee5be98238a54fbf7861fcc83c6e3cc91d334b7903d4f208e6`.

## Placement and 16-user smoke

Substitute two available distinct CPU numbers. The probe sends 24 round trips and verifies both sides’ accepted receive CPU through `SO_INCOMING_CPU`. The smoke then opens real connections to the submitted server with one stock Go process on the client CPU. Use a fresh output name for each run.

```sh
server_cpu=1
client_cpu=2
python3 probe.py placement --work "$work" --tokens "$tokens" \
    --server-cpu "$server_cpu" --client-cpu "$client_cpu" --manager-cpu "$client_cpu" \
    --out "$work/placement"
python3 run.py --work "$work" --seed "$seed" --tokens "$tokens" --server "$server" \
    --server-cpu "$server_cpu" --groups "$client_cpu" --manager-cpu "$client_cpu" \
    --targets-per-client 8 --users 16 --ramp 2 --hold 6 --sample 1 \
    --out "$work/smoke-16"
python3 summarize.py "$work/smoke-16"
```

The packaged smoke opened all 16 sockets on both sides, completed 23 requests with zero errors and drained all eight server namespaces. The placement probe observed all 24 receives on the selected CPU at each endpoint. Request counts in a short smoke depend on the scorer’s randomized think times.

## Full scored schedule

The command below uses the original four-shard topology, a 60-second ramp and a 300-second hold. Adjust the example CPU numbers for the host and reserve the cores for the whole run. Comparing different binaries requires a fresh seed copy and a new output directory for each run. The driver copies and hashes the seed inside the native server’s memory scope before startup.

```sh
python3 run.py --work "$work" --seed "$seed" --tokens "$tokens" --server "$server" \
    --manager-cpu 15 --server-cpu 10 --groups '0,1,2;3,4,5;6,7,8;11,12,13' \
    --targets-per-client 2 --aliases 12 --users 350000 --ramp 60 --hold 300 \
    --memory-mib 1750 --out "$work/veth-350k" --verify-close
python3 summarize.py "$work/veth-350k" > "$work/veth-350k/summary.json"
```

Each `go*/score.json` is the stock client output. The stock client’s merge mode combines all completed requests during ramp and hold in `score.json`, including failures, HTTP latency, connection latency and its pass decision. `shape.json` records the schedule, source tuples, hashes and fd limits. `samples.jsonl` records the native and client cgroups, private TCP counts, interface drops, per-CPU softirq, memory events and host conntrack. The summary distinguishes memory peaks sampled by the scorer cutoff from values observed afterward. If the user journal is available, `service-journal.log` retains scope OOM events and the summary dates them relative to the cutoff.

After the stock reports close, the driver keeps the private links alive through normal TCP teardown. `drain.json` records completion or a timeout. The default final-drain timeout is 120 seconds and can be set with `--drain-timeout`. Host TCP allocation counters include unrelated traffic. The driver removes its labeled containers and ephemeral image, temporary control sockets and database. The requested measurement directory remains.

Add `--verify-close` to observe the server through the concurrent shutdown of all stock clients. `closure.jsonl` records native cgroup memory, file cache, sockets, slab, dirty/writeback, reclaim, OOM counters and PID liveness. Samples are approximately 100ms apart during the final four seconds and through shutdown, and one second apart earlier. Non-touching `mincore` scans record resident pages in the physical database and WAL around startup and one second before cutoff. The independent observer runs on the manager CPU. `shape.json` records the copy hash measured inside the server cgroup before `exec`; the harness checks this value without reading the database again after the application opens it.

After all stock Go subprocesses have exited, the driver leaves all private links up to observe native close handling. `close-tcp.jsonl` records `Tcp.CurrEstab` per server segment and client namespace. The Linux counter includes both ESTABLISHED and CLOSE_WAIT. When the original PID survives, the driver sends a separate, ordinary `/health` request to each allocated native listener over its real private client link. These requests fall outside the reported stock score. `post-close.json` records all health results, the process state before intentional termination and read-only full SQLite integrity, foreign-key and row-count checks on the same physical database both before and after termination. The integrity scans can bring additional file pages into cache. Their UTC start time identifies this phase separately from client shutdown and the scored memory window.

SQLite’s `unix-excl` VFS can return `SQLITE_BUSY` or `SQLITE_LOCKED` to the independent reader while the native process owns the database. The report records the SQLite code and still requires original-PID health on every listener and a successful full integrity and foreign-key check on the actual database after normal owner termination. An early native exit, unhealthy listener, integrity failure, or unexpected SQLite error fails the shutdown verification.

The verifier requires a successful exit from every original stock shard, zero pre-health `Tcp.CurrEstab` on every native and client segment, and the same database inode in both independent checks. Negative lifecycle verification returns a nonzero process status after writing all raw score files, recording the scope journal when available and cleaning up its private resources.

With `--verify-close`, the network providers pass one already-open, read-only `/proc/net/snmp` file descriptor per dedicated network namespace to the manager. The manager samples `Tcp.CurrEstab` for every native and client segment together with the cgroup measurements during shutdown, including when the native service exits. The helper processes do not run at the sampling frequency and no additional TCP requests are sent.
