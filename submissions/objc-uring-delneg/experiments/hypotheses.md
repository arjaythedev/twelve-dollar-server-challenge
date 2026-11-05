# Hypotheses, objc-uring-delneg, 5 Nov (ObjcTop). Append-only: status changes are new lines at the end.

Goal: hold more than 310,000 (rank 1). Official: 280,000 at `1fe6941`, 290k/300k/320k ~77 % connection refused.
Harness `hs.sh` on Synteq (see worklog C0): systemd unit as `deploy`, LimitNOFILE=65535, MemoryMax=1750M, CPU 0, softirq on
CPU 0, memory hog leaving a 2 GB box's memory, ul2 on CPUs 1-7.

Proven before the list was written (C0, `1fe6941`, 300k): global OOM killer kills the server 70 s after the ramp ("Out of
memory: Killed process (server)", result oom-kill, 63 % errors, 2.78M connect errors). Order of events: file cache evicted
(cgroup file 10 MB), loop in D state reading the DB, requests pile up in socket queues (cgroup sock 222 → 904 MB, TCP 57k →
217k pages), anon 206 → 260 MB, kill. No exit of our own, no accept failure.

| id | idea | assumption it challenges | expected | rule check | cost |
|---|---|---|---|---|---|
| H1 | Bounded re-arm after ENOBUFS: FIFO, per ring never more than free buffers per pass | re-arming every ended multishot recv in one pass is harmless | breaks the recv storm at the wall (#245, #264 report it as what ran them out of memory) | own process only; idea credited | 40 lines, 2 runs |
| H2 | Output buffers 16 KB → 4 KB, larger free list; a burst no longer mallocs 16 KB per response | per-connection 16 KB malloc is cheap | anon spike 206-260 MB → ~100 MB, +25k users of room | fine | 10 lines |
| H3 | SQLite page cache 32 → 64/96 MB so the hot set is anon, and the loop stops reading the DB from disk when the file cache goes | 32 MB is enough because the OS caches the rest | no D state on the loop at the wall, trade ~60 MB | rule 5 allows SQLite's own page cache | 1 flag |
| H4 | mlock our code and libs (≤ 8 MB RLIMIT_MEMLOCK) | code pages stay resident | fewer major faults on the loop | own process; #245, #257 do it; no kernel knob | 30 lines |
| H5 | Checkpointer child at nice 10 | a child at nice 0 is harmless on 1 vCPU | shorter loop preemption; #278 says nice 19 starves, 10 fine | setpriority on own process is not pinning | 1 line |
| H6 | 8 SO_REUSEPORT listeners | one accept queue is enough | fewer SYN retransmits at the end of the ramp | own sockets | 20 lines |
| H7 | Rings created lazily instead of 8 up front | up-front rings cost nothing | ~10-20 MB kernel + anon | fine | 20 lines |
| H8 | Smaller TDConn record (stride now 96+ B) | 28 MB at 300k does not matter | ~10 MB | fine | 10 lines |
| H9 | WAL hold at 4,096 frames instead of 8,192 | 32 MB of WAL in cache is free | 16 MB less file cache needed, more holds | rule 6 fine | 1 flag; earlier 300k A/B found no gain off the wall |
| H10 | Drain first: read everything, answer ≤ N per pass, parked in conn buffer | answering at once is best in a burst | socket queues empty in < 1 s after a stall | fine | 80 lines; only if H1 isn't enough. #319 did this on Mojo and scored lower, so test hard |
| H11 | Poll + single read instead of multishot recv on idle sockets | 360 B of kernel state per idle socket is unavoidable | −30 MB at 300k, +10 % CPU (#245, #264 numbers) | fine | large; last resort |
| H12 | Drop the DB's file pages from the cgroup after startup (fadvise) | fresh copy's cache is useful | little; the wall evicts them anyway | own file | 5 lines |

Rejected up front: read coalescing across requests (rule 5 open question), accept guard on MemAvailable (#319 says it
caused its own connect timeouts; #112 flagged for /proc/meminfo), closing idle connections (POSTs land on closed sockets,
#277), raising oom_score of helpers or sacrificial processes (benchmark gaming), restart loops in start.sh (rule flag).

--- LIST LOCKED 2026-11-05T11:20Z ---
