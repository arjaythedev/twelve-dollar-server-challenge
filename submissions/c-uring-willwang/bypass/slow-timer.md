# Sparse 500 ms TCP timer

The optional `bin/server-bypass-slow` builds on the submission’s indexed, fast-250-ms TCP stack. lwIP 2.2.1 supplies the TCP state machine, packet parsing, congestion control, acknowledgments, retransmission, flow control, keepalive and close processing. The contribution in this variant is a deadline calendar for *when* to run lwIP’s existing 500 ms logic. The original release and both earlier patches remain separately buildable. [Scheduler patch](lwip-slow-work.patch) · [lwIP license](LICENSE.lwip)

The calendar enrolls this application’s live connections after the port sets keepalive and its poll interval. A fully idle established peer can sleep until its next poll or keepalive deadline, up to 127 ticks. Input, writes, output, close and poll changes wake it. The scheduler materializes the elapsed idle poll increments before a wake. Raw applications that opt in must wake a peer before changing its keepalive fields directly or inspecting the backing poll counter. The challenge port configures those fields before enrollment. Queued or refused data, retransmission, zero-window persist, short detached polling and transition states continue on the ordinary every-tick path. The TIME_WAIT pass retains upstream traversal.

A 256-slot wheel handles registration and removal in constant time. During a sparse tick the scheduler sorts the due peers by their existing active-list rank to preserve upstream order. It uses a 32,768-entry scratch array and falls back to the ordinary active list for larger or dense cohorts. Before invoking an application poll or error callback it exposes the same partially advanced counters and resumes upstream traversal, so callbacks can register, abort or wake another connection. A bounded refresh prevents an old eight-bit visit marker from aliasing after counter wrap. The port uses the single-threaded IPv4 raw API and a non-reentrant netif output callback.

## Reproduction

Run from the submission directory. The first command builds two independent stack layouts against the same application and transport. The test only uses in-memory IPv4 packets and the original TCP functions.

```sh
make -f Makefile -f bypass/slow.mk bin/server-bypass bin/server-bypass-slow
bypass/test-slow-work.sh vendor/lwip-2.2.1
CFLAGS='-O3 -g -std=gnu11' bypass/test-slow-work.sh vendor/lwip-2.2.1 350000 9000 4
```

A separate optional command runs lwIP's original 56 TCP, TCP_OOS and TCP_STATE tests against stock, fast-250-ms, and calendar builds. It requires the full pinned source archive including `test/unit`, and the C Check development library discoverable with `pkg-config`. The usual application fetch stores the smaller `src` tree.

```sh
bypass/test-lwip-upstream.sh /path/to/lwip-STABLE-2_2_1_RELEASE
```

The packet regression compares frame order, virtual tick, TCP flags, sequence and acknowledgment numbers, payload bytes, callback mutation and connection reuse. It exercises actual retransmission and congestion backoff, a closed send window and persist probe, out-of-order expiry, both FIN states, TIME_WAIT, SYN expiry, poll-counter catch-up and 32-bit tick wrap. A separate production-listener test drives ten virtual minutes, including a lost keepalive, a silent peer, a closed port and FIN backpressure. The 128-peer moving case checks every outgoing data byte.

## Local CPU fixture

GCC 13.3, one CPU, 350,000 shuffled established PCBs and 9,000 request/response exchanges per simulated 250 ms tick yielded the following results over 36,000 exchanges. This clock covers TCP pbuf processing and both timers. Full HTTP runs additionally pay for the network, HTTP and SQLite.

| Per run | Fast-250-ms control | With slow calendar |
| --- | ---: | ---: |
| 500 ms timer, CPU ms per call | 69.548 | 3.596 |
| Packet processing, CPU µs per exchange | 1.449 | 1.521 |
| Entire fixture, CPU µs per exchange | 5.469 | 1.828 |
| LP64 PCB size with keepalive | 288 B | 312 B |
| Process peak RSS | 110,464 KiB | 115,968 KiB |

Both arms produced 69,332 outgoing TCP segments, received 5,967,531 request bytes and produced the same segment trace digest `06b3c2474e574323`. The separate 650-tick mutation and protocol regression produced digest `d8550ed3e250d860` in both. The larger PCB increased glibc’s allocation by 16 bytes per peer on this host. A synchronized cohort can still require a full list scan.

At the scratch-array boundary, 17,190 and 17,191 exchanges per 250 ms tick made 32,768 and 32,769 distinct peers due in one 500 ms interval. With 350,000 connections, the calendar tick took 7.83 ms just below the cutoff and 60.68 ms just above it. The control took 63.09 and 56.92 ms on the respective streams. This was one timed tick per condition. The packet trace matched between builds at both sizes. [Scheduler and cutoff](lwip-slow-work.patch) · [Reproduction fixture](test-slow-work.sh)

## Full HTTP run

The local 350,000-user stock-Go trial used one server CPU, a 60-second ramp, a 300-second hold, generic AF_XDP receive and packet GSO transmit. The scheduler build `e2839141` served 22,235 successful requests/s with 5.70% failures and 20.20 s p95. The earlier `ddf68d7c` fast-250-ms hybrid served 23,868 successful requests/s with 4.65% failures and 6.37 s p95. Both missed the challenge thresholds. The calendar executable reused all nine application and transport objects from the frozen `61fcf2d5` release. The 350k reference run used the earlier ELF.

Through second 185 the calendar had completed 55,424 more successful requests. All 32 client shards then fell behind the control. Over the next ten seconds, the calendar delivered 19,730 successful requests/s against 24,204/s for the control. The Linux clients recorded 39,292 TCP timeouts/s against 27,880/s. The benchmark captured no per-tick scheduler counters, leaving the trigger for this loss unresolved. [Full Ethernet measurements and scorer](../tests/bypass/README.md)

## Optional live counters

The original calendar ELF has no timer diagnostics enabled. Build the isolated telemetry overlay with the following command.

```sh
make -f Makefile -f bypass/slow-stats.mk bin/server-bypass-slow-stats
```

Send `SIGUSR1` to this process to sample cumulative `lwip slow` fields from its event loop. `sparse` and `dense` classify a tick at entry. `callback_fallback` counts ticks that started sparse and switched to a full traversal before an application callback. `callback_sync` also includes dense ticks whose callbacks needed their peers’ poll counters exposed. `due_total` and `active_total` sum the starting populations. `active_visits` includes repeated visits after a callback restarts the walk. `sync_visits` counts peers seen in callback synchronization and `timewait_visits` counts the separate TIME_WAIT pass. The cumulative and maximum `wall_ns` and `cpu_ns` cover the entire slow timer. The diagnostics read both clocks on entry and exit once per tick.
