# Delayed TCP ACK deadlines

An established connection can send a partial HTTP request and then wait for a TCP acknowledgement before sending more. The existing lwIP fast timer issues delayed ACKs on a 250 ms tick. An ordinary Linux sender can retransmit its unacknowledged segment before that tick.

The optional fast-wide build registers a 40 ms deadline when the raw TCP receive callback accepts data and lwIP requests a delayed ACK. Committed application replies can acknowledge those bytes first. The queue cancels their tickets when output succeeds. The usual immediate second-segment ACK, receive window, out-of-order handling, retransmission and FIN processing stay with lwIP. The stock 250 ms timer can also satisfy a deadline first.

Each connection holds a 16-bit ticket in existing padding. The bounded FIFO uses 131,104 bytes on the measured x86-64 build and reserves 8,192 slots across the server. On exhaustion it requests an immediate native ACK. A dispatch services at most 64 expired slots and prioritizes ready HTTP output. When the link refuses an ACK, the dispatcher allows other work to proceed and retries the affected ticket in 40 ms. The slot is unlinked before the application's connection is freed, including when lwIP has already destroyed its PCB.

Build the optional binary and run the queue and wire tests from the submission directory.

```sh
make -f Makefile -f bypass/ack-deadline.mk bin/server-bypass-fast-wide-short-ack check-ack-deadline
```

The wire test executes the production dispatcher and real lwIP against a controlled clock. It covers a partial request, immediate second-segment ACK, a piggybacked response, duplicate and out-of-order segments, FIN and a refused transmission followed by recovery. The queue test covers clock wrap, cancellation after PCB destruction, slot saturation and reuse, and per-pass fairness.

To reproduce the Linux partial-request comparison, run `tests/bypass/tx_burst.py` with the matching control and short-ACK binaries in ABBA order. Specify `--connections 2048 --tx-frames 4096` and use identical private server, client and observer cores for each invocation. Each run sends three partial-request fragments, samples each Linux socket's `TCP_INFO`, then verifies all 2,048 completed health responses.

## Local measurements

With 2,048 Linux peers, the unchanged fast-wide control recorded 2,816 and 2,217 retransmissions across its three fragment turns per run. Both short-ACK runs recorded zero. All four runs verified 2,048 successful health responses and zero refused link transmissions.

The ordinary HTTP control alternated requests for the seeded feed and an individual post over 64 established Linux connections, with 4,096 warmups and 24,576 byte-verified replies per arm. In ABBA order the server process consumed 27.186, 26.895, 26.779 and 27.239 microseconds per response. All four runs had zero peer retransmissions. The two controls transmitted 49,165 packets and the two short-ACK runs transmitted 49,160. These are short paired measurements of two read routes. [Per-arm CPU, frames, retransmissions and correctness](../tests/bypass/ack-results.json)

## Narrow-table and 512-slot comparison

The separate `bin/server-bypass-fast-short-ack` target links the same deadline port to the ordinary 17-bit TCP flow table. The CLI and every object other than the deadline port match `bin/server-bypass` from the same source build. Run `make -f Makefile -f bypass/ack-deadline.mk bin/server-bypass-fast-short-ack check-ack-deadline-narrow` to build it and exercise the native queue and wire tests.

With 8,192 receive slots and 512 transmit slots, 2,048 Linux clients sent three synchronized partial-request fragments in each run. The controls recorded 4,608 and 4,608 retransmissions. The short deadline build recorded 1,753 and 3,072. In all four runs, the number of refused sends equaled the number of Linux retransmissions. All 8,192 subsequent health requests succeeded. Upstream's 250 ms fast timer clears both ACK flags after trying to emit an ACK, including on a refused send. An outstanding short deadline may then observe cleared flags.

Both binaries passed the organizer's 42 cases on the private Ethernet interface and the same 42 on localhost, then the 13 independent SQL cases. The short-deadline binary also passed the blocked-WAL, disk-full rollback, process-kill recovery, socket admission and half-close tests with the same ring capacities. [Narrow and wide results](../tests/bypass/ack-results.json)

## Retrying a refused ACK on the native fast timer

On a full transmit ring, lwIP's fast timer calls `tcp_output` and then clears the connection's delayed and immediate ACK flags even if transmission failed. The optional narrow-table retry build clears those flags only after `tcp_output` succeeds. The existing 40 ms deadline can then send the still-pending ACK. The existing output retry interval, dispatch budget and timeout behavior also apply when the peer sends no further data.

```sh
make -f Makefile -f bypass/ack-output.mk bin/server-bypass-fast-ack-retry check-retain-ack
```

The deterministic test sends an incomplete request at 232 ms, refuses the 250 ms native ACK, then observes the application's 272 ms deadline. The unmodified short-ACK control emits zero ACKs there. The retry build emits one with the correct sequence number. These builds use the same canonical source path for compiler assertion strings. Omitting the return check reproduces the control binary byte for byte.

The two 2,048-peer Linux control runs recorded 2,809 and 184 retransmissions. Both retry runs recorded zero, including one with 215 refused sends. Across the six retry turns, the observation sweeps reached p99 between 66.6 and 72.2 ms. Four control turns exceeded 230 ms. All four arms returned 2,048 verified HTTP responses.

The initial 64-peer HTTP ABBA used 4.76% more issuer CPU per verified reply on average with the retry. A reverse BAAB repeat measured 0.31% more. Each experiment completed 98,304 byte-checked feed and post responses with zero peer retransmissions or link drops. Across all eight arms the difference was 2.48%. [Per-arm inputs and observations](../tests/bypass/ack-results.json)

At 285,000 configured users the corrected 17-bit/512-slot build missed the stock generator's latency limits. Across its full 60-second ramp and 300-second hold it served 25,350 successful requests/s, with 0.768% errors, 933 ms p95 and 19.8 s p99. Peak observed Linux clients were 284,478. The previously published fast-timer binary with the same table and ring sizes passed at 285,000 users. The microbenchmark's ACK recovery improvement did not yield a higher concurrency score. [Stock score and physical measurements](../tests/bypass/ack-results.json)

The public `bypass/test-lwip-upstream.sh /path/to/full/lwip-2.2.1` test also builds the retained-ACK protocol source using lwIP's assertion and leak-check options. The upstream TCP, TCP_OOS and TCP_STATE suites passed all 56 cases on each of the stock, fast, calendar and retained-ACK sources, with zero failures or errors.
