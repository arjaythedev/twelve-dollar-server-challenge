# Userspace TCP experiments

## Retaining HTTP responses for retransmission

The release gives lwIP a copy of each committed response. We tested retaining the immutable HTTP buffer and referencing it from TCP segments. Each payload pbuf held the response until TCP and any pending ARP queue released their references. The connection could queue the next pipelined response immediately. Retained allocations had a 32 MiB allowance within the existing 64 MiB output budget, with the regular copy path available when that allowance filled. [lwIP send-buffer ownership](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp_out.c#L362) · [ACK processing](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp_in.c#L1101) · [ARP queue ownership](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/ipv4/etharp.c#L1014)

Using separate payload descriptors increased server process CPU from 26.114 to 27.227 µs/request in a four-round test. A second version placed the writable TCP header, retransmission segment and both pbuf descriptors in spare space at the end of the response allocation. The sender wrote the header and body directly into its AF_PACKET GSO ring.

On Linux 6.8.0-147-generic, 64 ordinary TCP clients on CPU 3 alternated between `GET /feed` and `GET /posts/:id` over a private 1,500-byte veth. The server on CPU 2 queried a copy of the seed’s newest 20 posts, their authors and their likes. Both arms ran in the same process on the same established connections. Six rounds of 24,576 requests used copy, retained, retained, copy, retained, copy. Clients verified every byte of the 5,176-byte feed and 398-byte point responses.

| Measured across three rounds per arm | lwIP copy | Retained response |
| --- | ---: | ---: |
| Feed / point requests | 36,864 / 36,864 | 36,864 / 36,864 |
| Successful HTTP responses | 73,728 | 73,728 |
| HTTP response bytes | 205,479,936 | 205,479,936 |
| Incoming veth packets | 110,593 | 110,595 |
| Original lwIP transmit frames | 184,340 | 184,350 |
| Queued veth transmit packets after aggregation | 73,750 | 73,762 |
| Elapsed seconds | 2.152 | 2.158 |
| Server process CPU, µs/request | 26.253 | 26.090 |
| Whole server-core CPU, µs/request | 28.347 | 28.483 |
| Mean round p95 / p99, ms | 2.023 / 3.638 | 2.075 / 3.651 |

Whole-core CPU included user, system, IRQ and softirq time. Both arms recorded zero client retransmissions, receive drops and output refusals. The retained arm emitted 147,456 payload slices during the measured rounds. Including warmups, all 150,528 descriptor groups fit within response allocations, all 37,632 owners were released, and retained capacity peaked at 270,336 bytes. The release keeps the copy path because retaining responses added ownership complexity and gave no measured whole-core saving.

A separate send-queue test forced an allocation failure after the first segment and verified rollback, partial retry, a segment spanning responses, sequence-number wrap, segment splitting, reset and an ARP reference outliving TCP. A two-ended lwIP fixture first refused and then lost an outbound data frame. Across 34 ordered requests, the peer held its receive window, received 235,009 verified HTTP/SQLite bytes and closed with FIN. All 30 retained responses were released.

The second executable’s SHA-256 was `455089d717cc5f6c8425d1baedf0ef29df3ff727685578031bd2547012a84d2d`. Its six-round result JSON had SHA-256 `0f6604d36eb2bd3459d5c612c8b9234689f65459a8f0cac5430533537239f013`.

## Established receive and cumulative ACKs

We tested TCP header prediction after normal checksum validation and flow lookup. It accepted the next sequence number on an established, option-free connection with an unchanged ACK and advertised window, open receive credit, and no pending transmit, reassembly or refused data. A cumulative ACK covering the complete transmitted flight could free its queued segments in one pass. The existing sender still updated congestion control and RTT. A pool of 64 owned 320-byte buffers served short IPv4 TCP packets, returning each slot only on its final reference. [lwIP TCP input and ACK processing](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/tcp_in.c) · [pbuf ownership](https://github.com/lwip-tcpip/lwip/blob/STABLE-2_2_1_RELEASE/src/core/pbuf.c)

Unmodified lwIP 2.2.1, the indexed timer control and the variant produced the same wire bytes, callback data and TCP-state fingerprint `84e8cdbf8f7f6ff1`. The 142 emitted packets covered sequence wrap, duplicate and out-of-order data, refused data, zero-window reopen, partial ACKs, a complete-flight ACK, a FIN behind a gap and 65 simultaneous retained packets. All 56 upstream TCP, out-of-order and state tests passed. The variant also passed the organizer’s 42 checks independently on Linux localhost and from an Ethernet peer.

| Test | Control | Variant |
| --- | ---: | ---: |
| Median transport CPU per request/response turn, 350,000 resident PCBs | 1.439 µs | 1.514 µs |
| Median server process CPU per HTTP reply, 64 Linux peers | 26.934 µs | 26.802 µs |
| Median elapsed time for 24,576 verified HTTP replies | 735.2 ms | 735.7 ms |

The first test used two control/variant/variant/control blocks. Each run selected 180,000 connections with a reproducible random sequence, fed a 42-byte request through Ethernet and TCP, alternated 240- and 3,670-byte transmitted replies, and cumulatively acknowledged 351,900,000 bytes. The 64-peer Linux test used one block, four independent launches, 4,096 warmup requests per launch and the same feed and point reads as the send experiment. All four Linux runs recorded zero receive drops and zero client retransmissions. Every measured XDP redirect ran on server CPU 12 and clients ran on CPU 13. The release keeps its existing established receive path. Control and variant executable hashes began `61fcf2d5` and `87f42bc6`.
