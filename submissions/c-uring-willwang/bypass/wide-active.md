# Active TCP index sizing

The optional wide index assigns 1,048,576 buckets to active IPv4 flows and 131,072 to TIME_WAIT. At 350,000 active connections this changes the average load from 2.67 to 0.33 PCBs per bucket. lwIP performs fewer dependent PCB loads when demultiplexing a packet. Both tables use the same per-process SipHash-2-4 key and retain the full tuple and input-interface comparisons, duplicate ordering, unresolved-tuple fallback, and original unlink/promotion logic. The extra table space is 7 MiB on a 64-bit platform, independent of the flow count. Each PCB keeps its previous layout. [Optional source patch](lwip-wide-active.patch)

Apply the patch after the existing indexed source. The build also supports applying it after the fast and slow timer patches. The active width defaults to `LWIP_TCP_TUPLE_INDEX_BITS`. The optional make target sets `LWIP_TCP_ACTIVE_INDEX_BITS=20` while leaving TIME_WAIT at 17 bits. [Build targets](wide.mk)

From the submission directory, build the normal calendar variant with the wide index, run its lifecycle checks, and optionally measure both index widths on a chosen idle CPU.

```sh
make -f Makefile -f bypass/wide.mk WIDE_BASE=slow bin/server-bypass-slow-wide
make -f Makefile -f bypass/wide.mk WIDE_BASE=slow check-wide-active
taskset -c 14 make -f Makefile -f bypass/wide.mk WIDE_BASE=slow measure-wide-active
```

Use `WIDE_BASE=fast` and `bin/server-bypass-fast-wide` for the existing fast-timer baseline. The CPU number is configurable through `taskset`. The local measurement uses established lwIP PCBs and checksummed Ethernet frames, with each simulated client maintaining its own sequence and ACK numbers. It sends a batch of distinct requests before acknowledging their alternating 240-byte and 3,670-byte responses. The 256-peer case also tears down and replaces one connection every 31 turns. Callback delivery, acknowledged bytes, per-peer sequence progress, and drained send queues are checked during both runs. The fixture measures TCP processing in memory. HTTP routing, SQLite, kernel I/O, and the organizer's network load generator are outside its timed region. [Fixture](test-wide-active.c)

## Local measurements

On one AMD EPYC Genoa core, the initial fast-timer comparison used 350,000 flows and 145,000 turns per arm. Every arm delivered 6,090,000 incoming bytes and acknowledged 283,475,000 outgoing bytes. The measurements below average two runs in opposite execution orders.

| Interleaved peers | PCB replacements | Original active table | Four cached pointers per bucket | Wide active table |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 0 | 1,619 ns/turn | 1,521 ns/turn | 1,482 ns/turn |
| 256 | 4,678 | 1,759 ns/turn | 1,633 ns/turn | 1,609 ns/turn |

The two larger tables each added 7,168 KiB to peak RSS. The optional build uses the wide table. The comparison source measured the same compiled GCC `.text` bytes for the new TCP object as the packaged width setting. The fast-timer raw sample SHA-256 was `36a4a24af13099343770fc4511c409cb1a0c1cc6274695ab3bc4ad18244071e3`.

A second run used the packaged Clang target with the calendar timer for both arms. At 64 interleaved peers, the reference measured 1,742 and 1,807 ns/turn, and the wide table measured 1,531 and 1,430. At 256 with replacements, the reference measured 1,634 and 1,710, and the wide table measured 1,535 and 1,566. All eight arms delivered and acknowledged the same byte counts. The raw sample SHA-256 was `c2da00efda888c42412ec2b3df2c2e0201f97f0a2dce273b39ad67cdff887752`.

The normal calendar-plus-wide ELF on this build host had SHA-256 `d345881cb83e96c9062fa4e4de7df97d9759faf7339355ba4fabc9f2c2623bd7`. The reference was `e283914176f550706bdfeb5c2c28832ebe7c87232899acf7e380c1247e014fdd`. All 29 input objects besides `tcp.o` matched byte-for-byte. The public width patch had SHA-256 `9e33dbd4e77e5679309075180430714a983b6b398f88cbcc9720886e69edebce`.

The ordered lookup fixture compared 120,000 decisions against lwIP's linear selector for active and TIME_WAIT lists, including duplicate tuples, bound interfaces, wildcard registration, register-before-tuple, removal and reuse. All 98,620 successful matches selected the same PCB. The usual handshake, transmit, retransmission, delayed ACK, FIN, reset, timeout, and tuple-reuse tests also passed for both widths. A separate run of the 56 unmodified upstream TCP, TCP_OOS and TCP_STATE tests passed on the wide index. [Existing TCP lifecycle tests](test-pcb-index.c)
