# Batch 1 results (2026-10-10/11)

> **Superseded by the [leaderboard](../LEADERBOARD.md).** This is the first run, on a DigitalOcean droplet; its numbers
> aren't comparable with the leaderboard's box. Kept for reference.

All 28 pull requests open on 2026-10-09: 26 benchmarked, 1 failed to start (#31 R) and 1 not run (#21, which by its own
README doesn't use SQLite).

**Machine.** The DigitalOcean $12 droplet from the video (1 vCPU, 2 GB, Ubuntu 24.04), with UFW on, as in the video. Batch 2 ran
on a different (faster) machine, so compare the two batches only through the anchors in [batch-2.md](batch-2.md).

**How each entry was scored.** Built from source with its own install.sh (root) / build.sh, checked with the 42-check test
suite before any load, then a 1,000-user warm-up. Load came from 4 load generators in the same region running
`bench/load.js` (feed → think → post → think → 15 % like, 2 % post), users split by each generator's capacity. A hold
passes only if it passes on every generator. Search: 2-minute probes at 5k, 10k, 20k, 40k, 64k, then bisecting to 1,000
users (500 below 6,000), then a 5-minute hold, stepping down until one passes. (The first 9 entries were searched with a
staircase instead and re-run on four generators.) **Score = most users that passed a full 5-minute hold** (60 s ramp, 5 min
hold, 30 s ramp-down) with p95 < 500 ms, p99 < 1 s, errors < 1 %.

**Commits scored.** Each PR's head at the time of the run. One PR was updated afterwards: csharp-aspnetcore-maks-march #27
(scored `7ac3fc3`, merged at `6bb4bc5`).

## Results

Utilization columns: the target's monitor (every 5 s), averaged over the scored hold's steady 5 minutes. "app" = the
submission's own cgroup; RAM = its PSS, peak. Sorted by score.

| # | Submission | PR | Front | Users held 5 min | req/s | p95 ms | p99 ms | err % | target CPU % | app CPU % | Nginx CPU % | app RAM MB (peak) | RAM free MB (min) |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | cpp-epoll_inmem-khalefa-ow | #9 | direct | **64,000** ᶜ | 5,875 | 366 | 910 | 0.33 | 68 | 62 | - | 336 | 1,388 |
| 1 | rust-epoll-scooter1337 | #19 | direct | **≥ 64,000** ᶜ | 5,950 | 65 | 224 | 0.11 | 52 | 42 | - | 98 | 1,387 |
| 3 | cpp-epoll-khalefa-ow | #8 | direct | **63,000** ᶜ | 5,809 | 214 | 718 | 0.11 | 74 | 68 | - | 163 | 1,358 |
| 4 | cpp-uwebsockets-tbsvttr | #6 | direct | **61,000** ᵏ | 5,582 | 124 | 374 | 0.66 | 70 | 64 | - | 117 | 1,341 |
| 4 | cpp-uwebsockets-v2-tbsvttr | #15 | direct | **61,000** ᵏ | 5,565 | 143 | 557 | 0.79 | 68 | 62 | - | 67 | 1,395 |
| 4 | zig-usockets-tbsvttr | #25 | direct | **61,000** ᵏ | 5,589 | 111 | 247 | 0.54 | 69 | 63 | - | 98 | 1,371 |
| 7 | typescript-bun-xxmohamed012 | #3 | direct | **47,000** | 4,381 | 96 | 356 | 0.00 | 72 | 67 | - | 150 | 1,356 |
| 8 | c-iouring-raoz | #26 | direct | **46,000** | 4,291 | 54 | 178 | 0.00 | 34 | 22 | - | 39 | 1,433 |
| 9 | rust-axum-tbsvttr | #17 | direct | **43,000** | 3,977 | 130 | 478 | 0.14 | 78 | 72 | - | 1,130 | 407 |
| 10 | cpp-drogon-hendy643 | #29 | direct | **42,000** | 3,917 | 57 | 186 | 0.00 | 64 | 58 | - | 627 | 940 |
| 11 | php-swoole-mattsplat | #24 | direct | **40,000** | 3,720 | 189 | 853 | 0.00 | 77 | 73 | - | 581 | 937 |
| 12 | php-swoole-tzimpel | #28 | direct | **39,000** | 3,633 | 119 | 747 | 0.00 | 82 | 77 | - | 133 | 1,388 |
| 13 | fortran-microhttpd-tbsvttr | #16 | direct | **38,000** | 3,532 | 251 | 944 | 0.00 | 85 | 80 | - | 692 | 847 |
| 14 | c-fio-tbsvttr | #5 | direct | **35,000** | 3,259 | 158 | 370 | 0.00 | 78 | 74 | - | 338 | 1,199 |
| 15 | csharp-aspnetcore-maks-march | #27 | direct | **32,000** | 2,972 | 346 | 730 | 0.00 | 93 | 90 | - | 475 | 1,143 |
| 16 | typescript-bun-tbsvttr | #4 | direct | **29,000** ˢ | 2,699 | 163 | 623 | 0.00 | 82 | 79 | - | 173 | 1,411 |
| 17 | csharp-aspnetcore-andrexx | #20 | direct | **24,000** | 2,236 | 86 | 262 | 0.00 | 96 | 92 | - | 463 | 1,183 |
| 18 | python-fastapi-tejasnafde | #10 | direct | **22,000** | 2,035 | 353 | 653 | 0.00 | 91 | 89 | - | 286 | 1,302 |
| 18 | go-nethttp-tbsvttr | #18 | direct | **22,000** | 2,006 | 143 | 318 | 0.50 | 78 | 73 | - | 581 | 1,040 |
| 20 | rust-axum-SvetlinZarev | #22 | **nginx** | **18,000** | 1,630 | 32 | 120 | 0.81 | 72 | 37 | 27 | 61 | 1,525 |
| 20 | python-granian-tbsvttr | #13 | direct | **18,000** | 1,675 | 205 | 475 | 0.00 | 85 | 83 | - | 303 | 1,312 |
| 22 | erlang-cowboy-tbsvttr | #11 | direct | **15,000** | 1,397 | 171 | 320 | 0.00 | 83 | 78 | - | 597 | 1,037 |
| 23 | ruby-iodine-tbsvttr | #14 | direct | **14,000** | 1,306 | 50 | 154 | 0.00 | 62 | 58 | - | 470 | 1,144 |
| (ref) | python-fastapi-cknutson12 (merged #1, re-run) | #1 | direct | **11,000** | 1,025 | 98 | 206 | 0.00 | 83 | 81 | - | 274 | 1,389 |
| 24 | python-litestar-przwr | #23 | direct | **12,000** | 1,112 | 443 | 977 | 0.00 | 79 | 76 | - | 225 | 1,421 |
| 25 | ruby-rails-tbsvttr | #12 | direct | **3,500** | 327 | 62 | 142 | 0.00 | 87 | 85 | - | 240 | 1,428 |
| 26 | ruby-rails-gambitboy | #2 | **nginx** | **3,000** | 279 | 105 | 697 | 0.00 | 83 | 74 | 7 | 205 | 1,454 |
| - | r-plumber2-josiahparry | #31 | nginx | **failed to start** | | | | | | | | | |
| - | cpp-epoll-memstore-khalefa-ow | #21 | direct | **not run** (not SQLite, by its own README) | | | | | | | | | |

ᶜ **The box's connection limit.** Each k6 user holds one keep-alive connection. The target's firewall (UFW) tracks every
connection in the kernel's conntrack table, sized 65,536 entries on a 2 GB box, and a direct server also has a 65,536
file-descriptor limit. 65,000+ users fail with `nf_conntrack: table full, dropping packet` (seen in dmesg). I capped the
search at 64,000; rust-epoll passed 64,000 cleanly (p99 224 ms, CPU 52 %), so its true capacity is higher but can't be
measured on this VM without tuning the kernel (rule 11). These entries are effectively tied at the VM's ceiling.

ᵏ **Their own connection cap.** All three tbsvttr entries hard-code `keepalive_limit = 60000`: past 60,000 open connections
every response carries `Connection: close`, k6 reconnects, closed sockets linger in conntrack (TIME_WAIT) and the table fills
early. Without that guard they'd likely reach ~64,000 like the others.

The merged #1 (python-fastapi-cknutson12, 12,000 in its original single-load-generator run) was re-run on this rig for
comparison: 11,000 (probes to 13,000; 5-minute holds failed at 13,000 and 12,000). Same code path as #10 except #10 strips
the FastAPI/Starlette layers between uvicorn and the handlers — that alone doubled it (22,000).

ˢ 2-minute probes passed up to 31,000, but 5-minute holds failed at 31,000 and 30,000 (p99 ~2.5 s with p95 ~200 ms: periodic
stalls; it lists `/proc/self/fd` up to every 100 ms to count connections). Scored at the highest 5-minute pass.

Not CPU-bound at its limit: **c-iouring-raoz** failed 47,000 on all load generators at once with the target at only 34 % CPU
(something inside it — it sheds load based on /proc/meminfo and has fixed ring sizes). **ruby-iodine** and **cpp-drogon** also
failed with CPU to spare (62 %, 64 %). On 1 vCPU most entries fail at 70–90 % average CPU: latency spikes before 100 %.

## Rule-compliance notes (from the code review of every PR, before running anything; please fix in a follow-up PR)

No caching across requests, schema changes, unsafe durability settings, or system tuning was found in any of the 26 run.
Flags worth a PR comment:

- **#21 cpp-epoll-memstore-khalefa-ow** — not run: its README says it doesn't use SQLite and keeps tables in memory (rules 2, 5).
- **#31 r-plumber2-josiahparry** — fails to start on a clean box: the `ragg` package (a plumber2 dependency, prebuilt binary from
  Posit PPM) needs `libwebpmux.so.3`, which install.sh doesn't install (`apt-get install libwebpmux3`). Also accepts HS384/HS512
  tokens (spec: HS256 only). CI never ran on this PR.
- **#22 rust-axum-SvetlinZarev** — install.sh installs Rust into /root/.cargo, so build.sh (run as a normal user) can't find
  cargo on a clean box. It built here only because another submission (#19) had left a global /usr/local/bin/cargo wrapper.
  Also: on the batch-retry path it replies before COMMIT (db.rs:229-235), a rule 6 edge case.
- **#26 c-iouring-raoz** — start.sh restarts the server whenever it dies from a signal (with oom_score_adj=1000), which would hide
  an OOM kill (rule 8). It never restarted during these runs (journal clean).
- **#27, #29** — accept a JWT without `exp`. **#4, #5, #6, #15, #25** close connections above 60,000 (the ᵏ note), **#3** above
  64,000 and **#17 rust-axum-tbsvttr above 40,000** (`min(40000, nofile - 128)`, main.rs:356) — which is probably what stopped
  it at 43,000 (it also used the most RAM, 1.1 GB).
- **#12, #14** (and #2) rebuild Ruby 4.0.5 from source into /opt/ruby on every install (no "already installed" check).
