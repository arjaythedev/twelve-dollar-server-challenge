# lwIP fast timer

The release `bin/server-bypass` and the control `bin/server-bypass-indexed` use the same application, transport objects and pinned lwIP 2.2.1. Both index TCP connections. The release also enables `LWIP_TCP_FAST_WORK=1` when compiling the TCP core and its application port. [Timer patch](lwip-fast-work.patch) · [Connection index patch](lwip-pcb-index.patch)

lwIP’s 250 ms timer scans every active connection for delayed ACKs, pending FINs and refused input. The active list is already ordered by its most recent registration or move to front. The patch records that order and the oldest connection that acquired work. The original timer walks the resulting prefix, in its existing order. A write or callback can extend the prefix while the timer is running, including when an older connection needs work. Retries carry into the next tick. The two bounds hold integers, so removal, timeout and PCB reuse leave no pointers to freed connections. Flags that clear before the next tick can cause extra visits. The 500 ms slow timer still scans every active connection for retransmission, keepalive, polling and timeout.

lwIP shares an eight-bit `last_timer` between its two timer functions. Normal scheduling makes a complete slow-timer pass every two fast ticks. If an embedding calls only `tcp_fasttmr()`, the patch forces a full walk after 128 generations to avoid a stale byte matching a wrapped counter. The 64-bit registration order also supports renumbering the active list on overflow. These changes use lwIP’s single-threaded `NO_SYS` core context.

Build and run from the submission directory with the same compiler settings for both variants.

```sh
make bin/server-bypass bin/server-bypass-indexed
bypass/test-fast-work.sh vendor/lwip-2.2.1
CFLAGS='-O3 -g -std=gnu11' bypass/test-fast-work.sh vendor/lwip-2.2.1 350000 9000 4
```

The regression forces repeated FIN allocation failure, ACK allocation and link failure, callbacks that refuse input and remove another active connection, marks on both sides of a timer’s current position, and 12 wraps of the eight-bit timer. The 128-connection case verifies every emitted response data byte. Both binaries must also match received-byte counts, output segment counts and the sequence/ACK/port/flags/length digest.

The optional moving test preloads shuffled, established PCBs using the release keepalive, no-delay and poll options. Every synthetic request enters through real IPv4/TCP input. It issues a 104–3,670-byte application write, sends it through TCP, and feeds back cumulative ACKs. The following single-core fixture used GCC 13.3 with `-O3` and 9,000 such requests per simulated quarter-second tick over 350,000 PCBs. Its clocks cover pbuf processing and both TCP timers, excluding SQLite, HTTP and the operating-system packet path.

| Fixed workload of 36,000 requests | Indexed control | Fast timer |
| --- | ---: | ---: |
| 250 ms timer, CPU ms per call | 55.443 | 0.729 |
| 500 ms timer, CPU ms per call | 56.840 | 55.719 |
| Fixture CPU µs per request | 10.671 | 4.450 |
| LP64 TCP PCB with keepalive | 280 B | 288 B |
| Peak RSS | 105,088 KiB | 110,464 KiB |

Both produced 69,332 output segments, received 5,967,531 request bytes and produced digest `06b3c2474e574323`. On this host the extra eight-byte rank crossed a glibc size class, using 16 more allocator bytes per PCB. The reported nominal timer CPU percentage assumes four fast and two slow calls per wall second. In a live stack, lwIP arms its next timeout 250 ms after the current callback returns, so slow callbacks and a busy event loop reduce the actual tick rate. Full HTTP capacity depends on the measured network path.

A separate full stock-Go trial targeted 350,000 users for a 60-second ramp and 300-second hold on single-core, generic AF_XDP. The 512-RX control delivered 19,242 requests/s with 13.77% failures. An arm combining the fast timer, 8,192 RX slots, 2,048 TX slots and bounded TX recovery delivered 22,917 requests/s with 6.22% failures. Both recorded a 59.75-second p95 and missed the challenge thresholds. This combined comparison measures all four changes together.
