# zig-iouring-harshitkhandelwal208

This submission serves HTTP/1.1 **directly** on Linux using Zig 0.17.0, io_uring and in-process SQLite 3.54.1. The benchmark runner supplies `HOST`, `PORT`, `SQLITE_PATH` and `JWT_SECRET`.

## Request path

- One event loop serves keep-alive sockets with multishot receives and kernel-provided buffers. It starts polling one ring and activates additional rings as their fixed-file tables fill. Eight tables provide 65,472 slots each. The accept policy reserves headroom for outstanding accepts. An idle connection that reaches peer EOF needs one direct close operation. The batch tracks up to one pending response or close per descriptor.
- Startup first attempts the existing 4,096-entry submission queues and 65,536-entry completion queues. If kernel resources prevent the full set from being created, it releases the partial set, waits one second for asynchronous kernel teardown, and retries all eight rings with a smaller completion-queue limit (32,768, then 16,384, then 8,192). All 523,776 descriptor slots remain available. Hosts that support the original configuration retain it; request handling and batching are unchanged. Startup logs report each ring's actual queue sizes.
- Under light traffic the loop wakes on the first completion. After a busy pass it targets 96 completions across all active rings and waits up to 800 µs for the balance. The wait rotates among rings. On kernels that signal deferred work through the submission queue, the loop skips system calls on quiet secondary rings.
- A feed request reads up to 20 rowids from the creation-time index in response order. It scans each leaf in place and uses SQLite’s cursor to cross pages or read overflow records. It counts likes in one index walk when the rowids occupy a compact range, otherwise it looks up each count. When both endpoints of a sorted leaf segment have the same post ID, the segment length gives the exact count for that ID.
- The feed and single-post endpoints combine each row seek with its payload lookup. Author lookups use SQLite’s current page keys to predict the child page or row slot. They verify the actual key or child bounds and fall back to SQLite’s normal search on a miss. They share a serializer that copies unescaped JSON strings in blocks and writes small decimal values directly. It emits each post while the post and author cursors still own their pages. Each cursor has separate scratch storage for overflow records. The feed has a prepared SQL fallback.
- Typical responses are written directly into a 16 KiB slab with space reserved for HTTP headers. Large responses grow into owned buffers. The response pool is demand-paged. Startup prefaults the first ring's receive and connection storage. SQLite fills its 64 MiB page cache on demand.
- Each authenticated write verifies the HS256 signature and expiry. Writes use prepared statements and group commit in WAL mode with `synchronous=NORMAL`. Responses wait for the batch result. A separate connection checkpoints the WAL in the background.

## Build and verify

Run the official HTTP suite from the challenge root with a generated seed.

```sh
bash test/run.sh submissions/zig-iouring-harshitkhandelwal208
```

From the submission directory, the following commands run the Zig suite and compare the built server with ordinary SQL on two temporary fixtures. The Python check exercises both dense and sparse post IDs, index page boundaries, overflow records and feeds larger than 64 KiB. It also posts a chunked JSON body, reuses the connection and checks that a client can finish reading its response after half-closing its socket.

```sh
zig build test -Doptimize=ReleaseSafe --summary all
python3 test_reads.py
```

The build trains the SQLite shim on a generated scratch database. Clang uses LLVM profile data and GCC uses its own counters. The Zig build compiles the shim directly when a usable profile is unavailable.

## Local 350,000-user hold, November 20

Both builds ran on one logical CPU of a Ryzen 7 7445HS workstation under a 2 GiB server cgroup with swap disabled. Four clients on separate cores used an isolated loopback network and a fresh copy of the seed for each run. The driver followed the challenge's read and write mix and think times. It opened one persistent connection per user, then measured a five-minute hold after the connection ramp. The baseline was the saved November 19 local build.

| Hold metric | Local baseline | Updated |
| --- | ---: | ---: |
| Simultaneous connections | 350,000 | 350,000 |
| Requests | 11,103,696 | 11,111,566 |
| Requests/s | 37,012 | 37,039 |
| Server CPU per request | 9.68 µs | 9.50 µs |
| p95 | 5.8 ms | 2.9 ms |
| p99 | 87.2 ms | 4.8 ms |
| Failed requests | 0 | 0 |

The updated build peaked at 1,595 MiB of charged server memory across startup, hold, and a 30-second connection ramp-down. The server stayed healthy and had zero memory-limit or OOM events. SQLite passed `quick_check`. All 115,650 acknowledged new posts and 871,109 acknowledged new likes, including warmup writes, matched the increase in persisted rows.

Each column is one run. An intermediate build reached the cgroup memory limit when 350,000 connections closed together. The baseline also used an abrupt close, so its startup-to-shutdown memory peak has different teardown conditions. The 350,000-user result comes from the local C driver. The organizer's remote k6 benchmark remains unverified.

## Earlier local comparison, November 19

The baseline was the working tree on 2026-11-19, including the six uncommitted source files present before this pass. Both binaries used Zig 0.17.0, ReleaseFast, native CPU targeting and a locally trained Clang profile. The client and server ran on the same Ryzen 7 7445HS workstation (Linux 7.3.4), with a single logical CPU for the server and a fresh copy of the challenge seed for each run.

These are medians of three runs per build in alternating order. The local mixed-workload driver used 1,800 persistent users, a 3-second ramp, an 18-second hold and a 0.009 think-time multiplier. Throughput includes the ramp. CPU time includes the server process and its checkpoint thread. RSS excludes kernel socket memory.

| Metric | Local baseline | Updated |
| --- | ---: | ---: |
| Requests/s | 19,177 | 19,488 |
| p95 | 2.09 ms | 0.94 ms |
| p99 | 2.91 ms | 1.72 ms |
| Server CPU per request | 10.70 µs | 11.14 µs |
| Peak server RSS | 79.6 MiB | 27.5 MiB |
| HTTP failures across three runs | 0 | 0 |

The updated build used about 4% more server CPU per request at this load while reducing p95 and p99. Two additional runs used the November 19 binary.

| Workload | Requests | Requests/s over the full run | p95 | p99 | HTTP failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| Repository k6 script, 2,500 VUs, 15 s ramp, 5 min hold, 30 s ramp-down | 87,578 | 247 | 0.654 ms | 1.93 ms | 0 |
| Local mixed driver, 64,000 users, 22 s ramp, 45 s hold, 0.09 think-time multiplier | 3,277,553 | 48,919 | 421 ms | 474 ms | 0 |

At a 0.16 think-time multiplier, the 64,000-user test ran twice per build in reversed order with a 22-second ramp and 40-second hold. The baseline recorded p99 of 5,485 ms and 128 ms. The updated build recorded 212 ms and 18 ms. All four runs had zero HTTP failures. The spread across runs limits the precision of a speedup estimate at this concurrency.

The accelerated run spread client connections across eight loopback source addresses to avoid exhausting local ephemeral ports. The challenge score requires a separate load generator and a five-minute hold on the organizer's VM.

The SQLite B-tree shim and background checkpoint design derive from the MIT-licensed Rust submission. [rust-iouring-kensac](../rust-iouring-kensac)
