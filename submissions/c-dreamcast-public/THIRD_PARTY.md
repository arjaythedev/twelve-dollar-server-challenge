# Third-party code

Nothing below is committed here except the two small reused files under `guest/third_party/`
and our patch files. `build.sh` downloads everything else at pinned versions.

| What | Where it comes from | Licence | How we use it |
|---|---|---|---|
| Flycast | github.com/flyinghead/flycast @ `1b63c8a3677c`, plus the submodules its Linux build needs (pinned by that commit) | GPL-2.0 (submodules: their own licences, all GPL-compatible) | the emulator; built from source by `build.sh` |
| `patches/flycast/*.patch` | ours, as changes to Flycast | GPL-2.0, like the code it changes | hostfs syscall, TCP port forwarding, see the patch header |
| KallistiOS | github.com/KallistiOS/KallistiOS v2.2.2 (sha256-checked tarball) | KOS licence (BSD-style), see its `doc/LICENSE.md` | the Dreamcast OS, network stack and libc glue linked into the guest |
| `patches/kos/*.patch` | ours, as changes to KallistiOS | KOS licence | TCP passive close goes to LAST_ACK |
| GCC 13.2.0, binutils 2.43, newlib 4.3 | GNU / Sourceware, via KallistiOS's `dc-chain` | GPL-3.0 (tools), newlib: BSD-style | the sh-elf cross toolchain; newlib is linked into the guest |
| SQLite 3.54.1 | sqlite.org amalgamation (sha256-checked) | public domain | the database, compiled for the SH-4 |
| `guest/third_party/jwt.c`, `jwt.h`, `jwt_mb.inc` | cursed-jwt `63bc323` (ours) | MIT | HS256 verification (its portable scalar path) |
| `guest/third_party/fbparse.h` | cursed-http-parser `be52489` (ours) | MIT; picohttpparser-derived rules, MIT | HTTP/1.1 request parsing (scalar backend) |

No Sega BIOS or firmware is used: Flycast boots the program with its HLE BIOS (reios) and its own
replacement BIOS font. KallistiOS's prebuilt ARM sound driver is replaced by an empty file at build
time, since nothing plays sound.

Our own code here (`guest/*.c`, `guest/*.h`, the scripts) is MIT, like the rest of the challenge
repository. It runs as a separate program inside the emulator and talks to it only through the
emulated hardware and the hostfs syscall.
