# C on a Sega Dreamcast (KallistiOS + SQLite, in Flycast)

**This is a joke entry.** The whole server, from the HTTP parser to the SQLite B-tree, runs as a
KallistiOS program on an emulated Sega Dreamcast: a Hitachi SH-4 (overclocked from 200 to 3,200 MHz) with
the 32 MB RAM mod, talking
to the world through an emulated Broadband Adapter. It passes all 42 checks. Getting it to survive
load meant fixing six bugs in KallistiOS's TCP stack and three in Flycast's network bridge (below).

- **Language:** C (gcc 13.2.0, `sh-elf`, `-m4-single`)
- **Framework:** none; KallistiOS 2.2.2 (the Dreamcast homebrew OS) for threads, TCP/IP and the BBA driver
- **Driver:** SQLite 3.54.1 amalgamation, compiled for the SH-4, with a custom VFS
- **Emulator:** Flycast `1b63c8a`, built from source with a small patch, HLE BIOS (no Sega BIOS)
- **Nginx or direct:** **behind Nginx**. The guest keeps at most 48 connections, not 15,000.

While it runs, the Dreamcast's video output shows a live dashboard: requests per second for the last
100 seconds stacked by endpoint, per-endpoint and per-status counters, the last few requests, the
latency of the last one and the free heap. On the scoring box it draws into an Xvfb nobody looks at;
locally, run with a display (or `DREAMCAST_SCREEN=1`) to watch it.

## How it fits together

```
k6 ─► Nginx ─► 127.0.0.1:$PORT ─► Flycast (host process)
                                    │ asio acceptor → picoTCP → emulated RTL8139 BBA
                                    ▼
                   ┌──── emulated Dreamcast (SH-4, 32 MB) ─────────────┐
                   │ KallistiOS TCP/IP ─► guest/main.c (poll loop)      │
                   │   fbparse.h (HTTP) · cursed-jwt (HS256) · api.c    │
                   │   SQLite 3.54.1 ─► guest/vfs_hostfs.c              │
                   └──────────────────────────│────────────────────────┘
                                              │ "BIOS" syscall at 0x8C00100A
                                    Flycast reios/hostfs.cpp: pread/pwrite/fsync
                                              ▼
                                        $SQLITE_PATH (+ -wal)
```

- **Networking.** Flycast already NATs the emulated Broadband Adapter through picoTCP and can forward
  inbound TCP ports, but only for a hard-coded list of games. Our patch adds `FLYCAST_TCP_FORWARD` /
  `FLYCAST_TCP_BIND`, so `$HOST:$PORT` on the host is forwarded to port `$PORT` on the Dreamcast
  (192.168.169.2, from picoTCP's DHCP). Local clients appear to the guest as 198.18.0.1, because
  KallistiOS would answer 127.0.0.1 on its own loopback.
- **The database file.** SQLite runs inside the Dreamcast (rules 1–3), so its page I/O has to reach
  the real `$SQLITE_PATH`. The patch adds a syscall to Flycast's HLE BIOS: the guest's SQLite VFS calls
  it to `pread`, `pwrite`, `fsync`, truncate and stat `$SQLITE_PATH` and its `-wal` file, so the file
  on the host is the database. The VFS has no shared memory, which SQLite allows for WAL only in
  `locking_mode=EXCLUSIVE`; the WAL index lives in the guest heap. `journal_mode=WAL`,
  `synchronous=NORMAL` (rule 6): every commit is written through to the host's `-wal` before the 201,
  and `xSync` is a real host `fsync` at checkpoints.
- **No caching** (rule 5): every request runs its query; only the prepared statements persist.
  SQLite's page cache is 16 MB of the 32 (3 MB on a stock 16 MB console).
- **Per request.** One KallistiOS thread polls the listening socket and up to 48 connections. At 48,
  a new connection makes room by closing the longest-idle keep-alive connection (Nginx keeps more
  idle upstream connections than we keep); if none is idle, it waits in the listen backlog. `/feed` and `/posts/:id` are the queries
  from SPEC.md; `POST /posts` uses `INSERT ... RETURNING`; likes use `ON CONFLICT DO NOTHING`.
- **Clock.** `created_at` comes from SQLite's `strftime('%f')`, and the VFS gets the host's clock
  through the same syscall, so timestamps are real UTC with milliseconds. JWT `exp` uses it too.
- **Reboots.** `start.sh` reboots the Dreamcast if Flycast exits, and a watchdog does the same if the
  guest's main loop stops logging its 5-second heartbeat for 30 s. (Polling `/health` instead rebooted
  a Dreamcast that was merely overloaded, since the probe waits in line with everyone else.)
  Committed rows are already in the host file, so a reboot loses only the requests in flight. With
  the fixes below, neither has fired in our runs.

### The bugs it took

Each was found under load, then confirmed with the instrument in brackets.

| | symptom | cause | fix |
|---|---|---|---|
| KOS 0001 | guest out of memory after ~900 connections | a passive close (client's FIN first) went through CLOSING and TIME_WAIT, keeping 16 KB of buffers for 30 s | go to LAST_ACK, as RFC 793 says |
| KOS 0002 | guest froze with ~34 connections open | lock-order deadlock: `poll()` held the poll mutex while taking socket mutexes, packet input the reverse (stall reporter in `main.c`) | `poll()` no longer holds its mutex while polling sockets |
| KOS 0003 | 2 s on some new connections | every SYN was dropped while `accept()` was setting up another connection, so the client waited out its SYN retransmit (pcap) | queue the SYNs of other peers during `accept()` |
| KOS 0004 | guest out of memory under overload | every socket the server closed first sat in TIME_WAIT for 30 s with its 16 KB of buffers (TCP state census) | free the buffers on entering TIME_WAIT; it only needs to re-ACK a FIN |
| KOS 0005 | connections reset under bursts | the listen backlog was capped at 32, and a SYN past it got a RST | backlog up to 128; drop a SYN past it (the peer retries), as Linux does |
| KOS 0006 | 2 s on ~5% of `/feed` responses | data that didn't fit the peer's window was only sent by the 2 s retransmit timer, never when an ACK opened the window (census of timer resends) | send queued data when an ACK makes room |
| Flycast | host heap corruption (double free) | picoTCP's `connect()` ignored a duplicate socket-tree key (Linux reuses loopback ports at once), so deleting one socket freed the other | fail with EADDRINUSE and retry with another port |
| Flycast | host crash, "jump to heap address" | picoTCP's timer heap holds ~1,000 timers; when it ran out, `socket_del()` freed a socket that `pico_tcp_input()` was still using (AddressSanitizer) | room for ~67,000 timers |
| Flycast | Flycast terminated ("shutdown: Transport endpoint is not connected") | the bridge called asio's throwing `shutdown()` on a host socket whose client had already gone | use the non-throwing overloads |

### Sent upstream

The fixes are also proposed upstream, rebased onto current KallistiOS `master` and Flycast
`dev`, where they were retested. Two bugs only exist on Flycast `dev` and turned up there: a
double free racing `pico_enqueue()` against the pico thread, and the asio shutdown crash,
which our older pinned commit also has.

- KallistiOS: [#1582](https://github.com/KallistiOS/KallistiOS/pull/1582) (LAST-ACK),
  [#1583](https://github.com/KallistiOS/KallistiOS/pull/1583) (SYNs during `accept()`),
  [#1584](https://github.com/KallistiOS/KallistiOS/pull/1584) (TIME-WAIT buffers),
  [#1585](https://github.com/KallistiOS/KallistiOS/pull/1585) (listen backlog),
  [#1586](https://github.com/KallistiOS/KallistiOS/pull/1586) (send on ACK). The `poll()`
  deadlock is already fixed on `master`.
- Flycast: [#2592](https://github.com/flyinghead/flycast/pull/2592) (`pico_enqueue()` double free),
  [#2593](https://github.com/flyinghead/flycast/pull/2593) (asio shutdown),
  [#2594](https://github.com/flyinghead/flycast/pull/2594) (duplicate connect),
  [#2595](https://github.com/flyinghead/flycast/pull/2595) (timer heap).

## Build

`install.sh` (root, once) installs the host compiler, CMake/Ninja, SDL2/Mesa, Xvfb and the
toolchain's build deps. `build.sh` builds everything from source into `build/`:

1. SQLite 3.54.1 amalgamation (sha256-checked)
2. KallistiOS 2.2.2 tarball (sha256-checked) + `patches/kos/` (the six fixes above)
3. the `sh-elf` cross toolchain (GCC 13.2.0, binutils 2.43, newlib 4.3) with KallistiOS's `dc-chain`.
   **This is slow: expect well over an hour on one vCPU.**
4. KallistiOS
5. Flycast at `1b63c8a` with the submodules it needs, + `patches/flycast/`
6. `guest/` → `build/dreamcast-sql.elf`

Nothing prebuilt is shipped: KallistiOS's prebuilt ARM7 sound driver is replaced by an empty file,
since nothing plays sound. Licences are in [THIRD_PARTY.md](THIRD_PARTY.md).

## Run and test

```bash
bash test/run.sh submissions/c-dreamcast-public   # from the challenge repo
```

`start.sh` writes a throwaway Flycast config (HLE BIOS, Broadband Adapter, no audio, no online
services), starts its own Xvfb when there's no `$DISPLAY`, and runs Flycast in the foreground. The
server answers `/health` a few seconds after `start.sh`.

## Numbers

![The Dreamcast's video output during a k6 run](dashboard.png)

*The Dreamcast's screen during the 5,000-VU run below, at 3,200 MHz with 32 MB: requests per second
for the last 100 seconds by endpoint (blue `/feed`, green `/posts/:id`, pink likes, orange creates),
47 of 48 connection slots in use.*

Measured locally in Docker on an Apple-silicon Mac (arm64 Linux, so Flycast's arm64 dynarec, not the
scoring box's x86-64 one), through `start.sh`, behind the challenge's `bench/nginx.conf`, with its own
k6 script:

```bash
k6 run -e BASE_URL=http://127.0.0.1 -e VUS=2500 -e RAMP_UP=20s -e DURATION=60s bench/load.js
```

| VUs | requests/s | median | p95 | p99 | errors |
|---|---|---|---|---|---|
| 2,500 | 206 | 11 ms | 20 ms | 28 ms | 0 % |
| 5,000 | 346 | | 30 s | 30 s | 12.0 % |

Those two rows are at the stock 200 MHz and 16 MB, where the emulated SH-4 tops out at roughly 300
requests/s. That turned out to be the only limit: the host CPU was mostly idle, and a faster network
tick changed nothing. So `start.sh` now runs the SH-4 at 3,200 MHz (`SH4_CLOCK`; our Flycast patch
stops Flycast resetting it to 200 MHz when the network adapter comes up) with the 32 MB RAM mod and
a 16 MB page cache. With Flycast pinned to a single core:

| VUs | requests/s | p95 | p99 | errors |
|---|---|---|---|---|
| 5,000 (800 MHz, 32 MB) | 409 | 26 ms | 140 ms | 0.02 % |
| 5,000 (3,200 MHz, 32 MB) | 410 | 19 ms | 24 ms | 0 % |

On keep-alive connections straight to the guest (one core), throughput by SH-4 clock was 317 req/s
at 200 MHz, 965 at 800, 1,137 at 1,600 and 1,328 at 3,200. Before the fixes above it failed at 150
users (p95 2.02 s, from KOS 0006) and froze or crashed soon after.

## Files

| | |
|---|---|
| `guest/main.c` | the poll loop, HTTP framing and the framebuffer dashboard |
| `guest/api.c` | the five endpoints: SQL, request JSON parser, JSON output |
| `guest/vfs_hostfs.c`, `guest/hostfs.h` | the SQLite VFS and the guest side of the syscall |
| `guest/third_party/` | cursed-jwt and cursed-http-parser (ours, MIT) |
| `patches/flycast/` | hostfs syscall + TCP forwarding (GPL-2.0, as part of Flycast) |
| `patches/kos/` | the six KallistiOS TCP/`poll()` fixes |
