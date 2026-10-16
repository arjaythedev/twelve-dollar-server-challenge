# go-lowlevel-janso123

| | |
|---|---|
| **Language** | Go 1.27.2, with cgo |
| **Framework** | none: own event loop and HTTP/1.1 parser on `golang.org/x/sys/unix` v0.48.0 |
| **SQLite** | 3.53.4, bundled in `github.com/mattn/go-sqlite3` v1.14.52 (build tag `cgo_sqlite`), plus a small C binding (`app/cfeed.c`) that calls the SQLite C API directly |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` (`0.0.0.0:80`) itself |

## How it works

The server is three OS threads. Each one owns its job and its own SQLite connection.

1. **Event loop** (one goroutine locked to one OS thread). It uses two io_uring rings with direct descriptors
   when available, otherwise normal io_uring or level-triggered epoll. It parses HTTP/1.1 (keep-alive,
   pipelining, chunked bodies, `Expect: 100-continue`, HEAD), verifies the JWT and serves every read.
   There is no goroutine per connection.
2. **Writer** (locked thread). Every `POST` goes into a bounded queue. The writer takes everything queued,
   runs it as one transaction (one savepoint per request), commits, and only then wakes the loop through an
   `eventfd`. The loop sends the response after that.
3. **Checkpointer** (locked thread). Copies the WAL back into the database file with
   `PRAGMA wal_checkpoint(PASSIVE)`, and occasionally `RESTART` to keep the WAL bounded (~25 MB high-water).

## What I used and why

Each choice below replaced something I measured to be slower on a 1 vCPU / 2 GB VM.

- **Own event loop instead of `net/http`.** With `net/http` every connection is a goroutine. At 40,000
  keep-alive connections that was ~1 GB of goroutine stacks, and on one core the garbage collector spent
  most of its time scanning them; p95 was over 5 s. The loop keeps a connection as an ~80-byte struct plus
  pooled buffers, so 40,000 connections cost ~60 MiB RSS in total and the GC has almost nothing to scan.
- **`x/sys/unix` and raw syscalls.** `accept4`, `read`, `write`, `epoll_wait` and `eventfd` directly, without
  the Go netpoller in between. `unsafe` appears only where the C binding hands buffers across cgo, and in
  the io_uring package (below).
- **C binding for the hot reads.** `GET /feed` and `GET /posts/:id` are one Go→C call each: the C code
  steps the prepared statement and writes the JSON straight into the response buffer. Through
  `database/sql` it was one cgo call per column per row plus reflection in `encoding/json`, and that was
  the largest item in the CPU profile.
- **Hand-written JSON encoders** for the small responses (append-style, no reflection).
- **Group commit on a writer thread** (rule 6 allows it). Writes no longer block reads, and several
  writes share one `COMMIT`. A failing request (e.g. a like on a missing post) rolls back only its own
  savepoint.
- **Checkpoints on their own thread.** When the writer ran the checkpoints itself, every checkpoint
  (1–3 s with its `fsync` on my VM disk) stalled every write queued behind it: every ~30 s, likes and new
  posts took up to 3 s while reads stayed under 20 ms. With a separate checkpointer, a `PASSIVE`
  checkpoint runs concurrently with commits, and p99 at 40k dropped from 107 ms to 20 ms.
- **`BEGIN IMMEDIATE`, retried on `SQLITE_BUSY` before any statement runs.** A `RESTART` checkpoint briefly
  takes the write lock. With a plain `BEGIN`, a write could hit `SQLITE_BUSY` mid-transaction and become
  a 500. Taking the lock up front, and retrying only while nothing has been written, makes it wait instead.
- **SQLite settings:** `journal_mode=WAL`, `synchronous=NORMAL` (the minimum rule 6 allows),
  `foreign_keys=ON`, `cache_size=-8192` (8 MiB page cache), `mmap_size=268435456` (the ~250 MB database is
  read through the OS page cache without copies). Prepared statements are reused.
- **Default Go GC settings, no memory limit.** At 40,000 connections the Go heap is ~10 MiB and the
  whole process ~60 MiB, so the collector has little to do and needs no tuning.
- **Build:** x86-64-v3 when the CPU has it, the baseline otherwise (see below).

### Network: io_uring with direct descriptors, epoll as fallback

Every client connection on epoll is a file descriptor, so one process can hold at most `RLIMIT_NOFILE`
connections (often 65,535). The server uses io_uring **direct descriptors** instead: accepted sockets go
straight into a file table registered with the ring and never get a normal file descriptor. The kernel
caps each ring's table at `RLIMIT_NOFILE`, but per ring, so the loop thread drives two rings, which gives
twice the slots. Locally, with a hard limit of 65,535, it held 100,000 keep-alive connections with 19
process file descriptors. Epoll stopped at 65,517.

- Accept is multishot straight into the table. Recv is multishot from a shared pool of provided buffers,
  so idle connections hold no receive buffer. Send, shutdown and close all work on the fixed slots.
- Requests and completions are tagged with a per-connection generation, so a late completion can never
  touch a slot that has been reused.
- At 30,000 VUs on my VM, io_uring and epoll performed the same: p95/p99 1.44/4.61 ms vs 1.43/4.31 ms,
  and process CPU 0.110 vs 0.117 s per 1,000 requests. io_uring needed 1.2 read/write syscalls per request
  where epoll needed 4.2. The real gain is past the descriptor limit.
- If io_uring or a feature it needs is unavailable (disabled, seccomp, old kernel), the server falls back
  from direct descriptors to plain io_uring, and then to epoll. The startup line says which backend is
  active (`network_backend=…`, plus `fallback_reason=…` if any).

All ring operations are Go on `golang.org/x/sys/unix`: raw `io_uring_setup` / `io_uring_enter` /
`io_uring_register` syscalls and `unix.Mmap`, with no liburing and no C. All io_uring `unsafe` is
confined to `app/internal/netring`, with each use documented, and its structs match the Linux 6.8 UAPI
header. The only other `unsafe` is the cgo pointer passing in the SQLite feed binding.

Logging is minimal: one `network_backend=` line at startup (with `fallback_reason=` if a ring could not
start), and a rate-limited `write_failure` line if a write ever ends in an error response.

### An Ubuntu 6.8 kernel bug I had to work around

On my Ubuntu 24.04 VMs (kernel `6.8.0-147-generic`), io_uring never came up: the server always fell back
to epoll with `provided buffer ring: invalid argument`. In Docker on a newer kernel it worked.

I narrowed it down with small probe programs on the VM. `IORING_REGISTER_PBUF_RING` returned `EINVAL` for
every valid request: every ring size, buffer group, setup flag, and both kernel- and user-allocated ring
memory. Everything else I need worked: multishot accept and recv, the sparse fixed-file table, direct
accept, fixed recv/send/close, sync cancel and `EXT_ARG` waits.

The cause is in Ubuntu's kernel source for that version (`linux-source-6.8.0`, 6.8.0-147). A backported
change inverted the check on the struct's reserved words:

```c
if (!memchr_inv(reg.resv, 0, sizeof(reg.resv)))   /* rejects the valid, all-zero case */
    return -EINVAL;
```

So the kernel rejects exactly the requests the ABI requires (reserved fields zero). It accepts them if a
reserved word is non-zero. The same inverted check is in unregister and status. This is a known Ubuntu
regression ([Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)) in the
6.8.0 kernels from about -136 on, and the droplet's Ubuntu 24.04 image may well have it.

The workaround keeps correct kernels on the correct path:

1. Register the buffer ring the valid way, with reserved fields zero.
2. Only if that returns `EINVAL`, retry once with `resv[0] = 1`. That passes on the affected Ubuntu kernels.
3. If that fails too, use epoll.

The server never unregisters the buffer ring on its own (it cancels and closes the whole ring), so the
broken unregister path is never hit. With the workaround on `6.8.0-147`, the server starts with
`network_backend=uring-direct` and passes `test/test.sh` 42/42.

### Build: x86-64-v3 with a probe, and a fallback

x86-64-v3 is the CPU level with AVX2, BMI1/2, FMA, F16C, LZCNT and MOVBE (Haswell / Zen 1 and newer).
Built for it, Go uses those instructions directly instead of checking for them at run time, and GCC can
use them in SQLite's C code. I have not measured the gain on its own and expect a few percent at most.

The risk is all-or-nothing: I don't know which CPU the droplet gets, and the Go runtime refuses to start
a `GOAMD64=v3` binary on a CPU without v3. The server would never become healthy. So `make` only builds
for v3 after a probe passes on the build box itself:

1. `v3probe/` is a tiny Go + cgo program. It is built with exactly the server's v3 flags
   (`GOAMD64=v3`, `CGO_CFLAGS=-O2 -march=x86-64-v3`) and then run.
2. If the CPU lacks v3, the Go runtime aborts it before `main` runs.
3. If it does start, its C part asks the CPU directly with GCC's `__builtin_cpu_supports("x86-64-v3")`.
   That covers every v3 feature, including whether the OS has enabled the AVX register state.
4. Only if both pass is the server built with the v3 flags. If the probe fails, the v3 build fails, or the
   machine isn't x86-64, `make` builds the baseline (`GOAMD64=v1`, `-O2`) instead.

v3 can therefore only make the build faster, never break it. `make baseline` skips the probe. Verified in clean
Ubuntu 24.04 containers with `install.sh` + `test/run.sh`, 42/42 each time: the v3 path on x86-64 (the
binary reports `GOAMD64=v3`), the fallback when the probe fails, the fallback when the v3 build itself
fails (binary reports `GOAMD64=v1`), and the baseline on arm64.

### Why `GOMAXPROCS=2` on a 1-vCPU box

`GOMAXPROCS` does not add a CPU. It is the number of threads allowed to run Go code at the same time; the
kernel still time-slices them on the one core. I set 2 because of how the three threads interact:

- **Two locked threads share Go work.** The loop and the writer are both locked to their own OS thread.
  With one P, every time the writer needs to run Go code (taking the batch, encoding responses) the P has
  to be handed from the loop's thread to the writer's and back: a futex wake and a context switch for each
  handoff, in both directions, thousands of times a second.
- **cgo calls don't give the P back immediately.** When the writer is inside SQLite (`COMMIT`, which
  can wait on the disk), it holds the only P until the runtime's monitor (`sysmon`) notices it has been
  in a syscall for a while and takes the P away. That takes from ~20 µs up to 10 ms, and during that time
  the loop cannot run. With two Ps the loop always has its own.
- **Measured.** Same mixed workload (reads + ~240 writes/s), locally on one core: p95/p99
  **4.68/12.37 ms with 1 P** vs **0.65/3.94 ms with 2 Ps**. With 2 Ps, `COMMIT` mean/max also fell from
  0.17/11.2 ms to 0.03/2.0 ms. It is one paired run, but the effect is in the direction the
  mechanism above predicts, and I saw no downside.

## Rules

- **3, schema:** `schema.sql` and the seed unchanged; only SELECT and INSERT against the given tables.
- **4, tests:** `test/run.sh` passes 42/42; responses are byte-for-byte what `test.sh` expects.
- **5, no caching:** every request reads SQLite while it is served. Only prepared statements and scratch
  buffers are reused. The JWT signature (HMAC-SHA256, `crypto/hmac`) is checked on every request.
- **6, durability:** WAL + `synchronous=NORMAL`. A 200/201 for a write is sent only after its `COMMIT`
  returned. Tested by `SIGKILL` under load: every acknowledged write was in the database after restart,
  and `PRAGMA integrity_check` returned `ok`.
- **11, no tuning:** nothing on the box is changed. `GOMAXPROCS` and the pragmas
  are settings inside my own process. `LockOSThread` ties a goroutine to an OS thread, not a thread to a
  CPU.
- **Overload:** if the write queue (4,096 entries) is full, or the WAL passes 256 MiB without a checkpoint
  making progress, a write gets `503` instead of growing memory without bound. This never happened in
  any test.

## Results

My rig: a server VM with 1 vCPU (Ryzen 7 1800X host, KVM), 2 GB RAM, no swap, Ubuntu 24.04 with kernel
`6.8.0-147-generic`, and k6 1.6.1 on a separate VM. The load is the unchanged `bench/load.js` with its
default profile: 60 s ramp, 5 min hold, 30 s ramp-down.

This exact submission, built with `build.sh` on the VM (it chose x86-64-v3), served by io_uring direct
descriptors:

| VUs | Req/s | p95 | p99 | Errors | Result | Process CPU per 1,000 req | Peak RSS |
|---:|---:|---:|---:|---:|---|---:|---:|
| 30,000 | 2,801 | 2.56 ms | 4.75 ms | 0 | PASS | 0.118 s | 55 MiB |

For comparison, the best run of an earlier build with the same threads and database code on epoll:

| VUs | Network backend | p95 | p99 | Errors | Result |
|---:|---|---:|---:|---:|---|
| 30,000 | epoll | 1.43 ms | 4.31 ms | 0 | PASS |

**Why I stop at 30k:** from 40,000 VUs up, my load generator failed rather than the server. k6 crashed
again and again (SIGSEGV inside k6, with the same flipped bit in a pointer each time, which points to
faulty RAM on the host), and once even at 30k. One complete 40k run of an earlier epoll build of this design
passed with p95/p99 3.1/20.1 ms and ~39 % CPU idle (it still had the checkpoint-contention bug fixed
by `BEGIN IMMEDIATE` above, which answered 0.005 % of requests, all writes, with 500s). At 30k the server uses about half the CPU and
~55 MiB, so I expect it to go well past 40k on the droplet, but I could not measure that on my hardware.
