#!/usr/bin/env bash
# Runs the server in the foreground. It reads SQLITE_PATH, JWT_SECRET, HOST and
# PORT from the environment, as required by SPEC.md.
set -euo pipefail
# Ring-owned descriptors (io_uring direct descriptors + multishot recv)
# remove the 65535 real-fd ceiling. The runtime keeps the classic real-fd
# path automatically if registration or the provided-buffer ring fails, and
# HPHP_DIRECT=0 forces it.
export HPHP_DIRECT="${HPHP_DIRECT:-1}"
export HPHP_BG_CHECKPOINT="${HPHP_BG_CHECKPOINT:-1}"
# When commits overtake a background checkpoint, briefly hold writes so the
# WAL tail is copied and SQLite can restart the log from frame 0. Without it
# the WAL grew past 200 MB under sustained load. HPHP_CKPT_RESTART=0 disables.
export HPHP_CKPT_RESTART="${HPHP_CKPT_RESTART:-1}"
# Optional: open SQLite through another VFS, e.g. HPHP_SQLITE_VFS=unix-excl
# (the process holds the database lock, so transactions take no fcntl()
# locks). Off by default: the box run with unix-excl + WAL coalescing at
# f1f8d45 held 230k with a worse tail than the default VFS at 250k, so this
# head isolates the runtime changes.
export HPHP_SQLITE_VFS="${HPHP_SQLITE_VFS-}"
# Write each commit's WAL frames with one pwrite through a thin wrapper over
# the selected VFS (reads, syncs and locks flush first). Set 0 to compare the
# old path; the default VFS itself remains unchanged.
export HPHP_SQLITE_WAL_COALESCE="${HPHP_SQLITE_WAL_COALESCE:-1}"
# Keep SQLite's request cache at the 64 MiB measured baseline. Larger caches
# and the index warm-up remain opt-in via HPHP_SQLITE_CACHE_KIB / HPHP_SQLITE_WARM.
export HPHP_SQLITE_CACHE_KIB="${HPHP_SQLITE_CACHE_KIB:-65536}"
export HPHP_SQLITE_WARM="${HPHP_SQLITE_WARM:-0}"
# Optional: parse requests directly from the selected io_uring buffer when they
# fit in one receive. Partial/multi-buffer requests promote to an owned string.
# Enable with HPHP_RECV_ZEROCOPY=1; the default copy path remains the baseline.
export HPHP_RECV_ZEROCOPY="${HPHP_RECV_ZEROCOPY:-0}"
# Store only partial receive buffers in the PHP map; idle sockets need no
# per-connection empty-string entry. Set 0 to compare the legacy layout.
export HPHP_SPARSE_BUFS="${HPHP_SPARSE_BUFS:-1}"
# Six rings x 65535 direct slots = ~393k table slots; the runtime falls back
# to one ring (and to the real-fd path) when setup fails.
export HPHP_RINGS="${HPHP_RINGS:-6}"
# Bounded ring-0 waits and a 512-deep single-shot accept queue are the
# direct-mode defaults. Set HPHP_ACCEPT_DEPTH=0 to use multishot accept.
export HPHP_RING0_WAIT="${HPHP_RING0_WAIT:-1}"
# Do not wait for the full ring-0 CQ batch while accepted work or completions
# are already queued. Set HPHP_RING0_BUSY=0 to compare the prior wait policy.
export HPHP_RING0_BUSY="${HPHP_RING0_BUSY:-1}"
# Reuse SQLite table-cursor positions for repeated B-tree rowid batches within
# one poll transaction. No payload/result survives the batch or a write.
export HPHP_BT_MULTI_CURSORS="${HPHP_BT_MULTI_CURSORS:-1}"
# Return to receive processing between bounded application batches. The 2048
# budget reduced tails versus 8192 on the one-vCPU Ubuntu 6.8 VM and passed
# a five-minute hold. Set 8192 to compare the previous budget.
export HPHP_ANSWER_BUDGET="${HPHP_ANSWER_BUDGET:-2048}"
# Match Rust's SQ-full path: publish queued entries without waiting for CQEs.
# Set 1 to compare the previous GETEVENTS flush policy on the same binary.
export HPHP_SQ_FLUSH_GETEVENTS="${HPHP_SQ_FLUSH_GETEVENTS:-0}"
export HPHP_ACCEPT_DEPTH="${HPHP_ACCEPT_DEPTH:-512}"
# Keep accept ownership on one ring at a time and hand over before its direct
# table reaches the reserved-slot watermark (leader-style bounded accepts).
# Set HPHP_ACCEPT_HANDOVER=0 to retain the prior multi-ring accept scheduler.
export HPHP_ACCEPT_HANDOVER="${HPHP_ACCEPT_HANDOVER:-1}"
# Optional same-binary experiment: rotate which ring's CQ is drained first.
# Mixed-load A/Bs were not consistent enough to enable this by default.
# Set HPHP_DRAIN_RR=1 to test it without changing the server source.
# Cap accepted TCP receive/send buffers through the listener; on Linux these
# options are inherited by accepted sockets, including direct fds. A same-binary
# A/B/B/A at 120k connections showed no CPU difference; set 0 for the kernel defaults.
export HPHP_SOCKET_BUFFERS="${HPHP_SOCKET_BUFFERS:-1}"
# Decode the fixed posts record inline in HolyPHP instead of making five
# per-column B-tree builtin calls. Set 0 to retain bt_cols/column accessors.
export HPHP_FEED_RAW="${HPHP_FEED_RAW:-1}"
# Decode only the username field needed by the feed from the local SQLite
# record; malformed, overflow, or unusual records use the generic B-tree path.
export HPHP_FEED_USER_RAW="${HPHP_FEED_USER_RAW:-1}"
# Resolve the feed's 20 author rows with one generic sorted B-tree batch seek
# instead of one cursor seek per author. Set 0 to compare the legacy path.
export HPHP_FEED_BATCH_USERS="${HPHP_FEED_BATCH_USERS:-1}"
# These typed fast-array accesses are used only with in-range feed-loop indices.
# Set 0 to restore runtime bounds checks while diagnosing array logic.
export HPHP_IARR_FAST="${HPHP_IARR_FAST:-1}"
# Every ring uses DEFER_TASKRUN and is entered with GETEVENTS on each pass, so
# completions are posted only when the loop asks for them; ring 0 waits for 256
# completions or 1 ms. Mirrored A/B/B/A on one CPU: -9% CPU per request at 130k
# connections (kernel 6.18) and about -5% on an Ubuntu 6.8 VM at 80k, with the
# same tail. HPHP_DEFER_ALL=0 HPHP_FORCE_ENTER=0 HPHP_BATCH_N=64 restore the
# previous mixed-ring loop.
export HPHP_DEFER_ALL="${HPHP_DEFER_ALL:-1}"
export HPHP_FORCE_ENTER="${HPHP_FORCE_ENTER:-1}"
case "$HPHP_RINGS" in
  ''|*[!0-9]*) ;;
  *)
    if [ "$HPHP_DIRECT" = "1" ] && [ "$HPHP_RINGS" -gt 1 ] && [ "$HPHP_RING0_WAIT" != "0" ]; then
      export HPHP_BATCH_N="${HPHP_BATCH_N:-256}"
      export HPHP_BATCH_US="${HPHP_BATCH_US:-1000}"
    fi
    ;;
esac
exec "$(dirname "$0")/bin/server"
