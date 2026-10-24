# Research: techniques in the other submissions

A survey of the merged submissions and the open pull requests (as of 2026-10-24), made before tuning this one. It is the input to [FINDINGS.md](FINDINGS.md), which records what was actually adopted and measured. Gains quoted here are the other entries' own claims, not our measurements.


Date: 2026-10-24. Sources: `upstream/main` @ `9a77dcc` (batch 2 results), 102 open PRs fetched as
`refs/remotes/pr/<n>` in `/home/ondra/Projects/twelve-dollar-server-challenge`. Read-only research, no benchmarks run.

Base URLs: repo = https://github.com/arjaythedev/twelve-dollar-server-challenge ,
PR n = https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/n ,
batch 2 = https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/main/benchmarks/batch-2.md

Our entry: https://github.com/sideeffffect/twelve-dollar-server-challenge/tree/scala-http4s-sideeffffect/submissions/scala-http4s-sideeffffect
(Ember behind Nginx, doobie + HikariCP + sqlite-jdbc, read pool + 1 writer, SQLite calls as `delay`, 1 CE worker, ~260 µs/req locally).

## 0. Context that drives every verdict

- Load mix per k6 iteration (`bench/load.js`): 1 feed + 1 post-by-id + 0.15 like + 0.02 create. So ~46% feed, ~46% post,
  ~7% like, ~1% create. **Writes are ~8% of requests. Auth (JWT) is only on writes.**
- Batch 2 box is ~2x faster per request than the DO droplet (anchors). Score is capped by: CPU (µs/req), memory per
  open connection (direct mode), fd limit 65,535 (direct mode), and Nginx's 16,384 `worker_connections` (Nginx mode).
- **Every probe restarts the app on a fresh DB** (batch-2.md). JIT warm-up happens inside each probe's 60 s ramp.
- Nginx config (https://github.com/arjaythedev/twelve-dollar-server-challenge/blob/main/bench/nginx.conf) has
  `gzip on; gzip_types application/json; gzip_min_length 1024;`. The feed body (~3-4 KB) is gzipped by Nginx on the
  same core, if the client sends `Accept-Encoding: gzip` (k6 does by default as far as I know; verify with
  `body_bytes_sent` in the bench access log). Noted by java-raw-agrison https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/101 .

### Batch 2 reference points (µs = app CPU / request)

| Entry | Users | µs/req | RAM MB | Mode / key trait |
|---|---:|---:|---:|---|
| rust-epoll-scooter1337 (#82) | 264,000 | 30 | 630 | io_uring fixed files, group txn, range feed |
| asm-nasm-melroy89 (#86) | 171,000 | 43 | 358 | io_uring, batch txn, wal-hook ckpt |
| go-epoll-MegaPanchamZ (#72) | 165,000 | 51 | 187 | epoll, PGO SQLite, LRU idle eviction |
| c-iouring-ssmurfgg04-gif (#112) | 150,000 | 50 | 413 | io_uring, group commit, range feed |
| kotlin-nio-3x3xX3N0N (#83) | 120,000 | 79 | 1,380 | 2 JVMs SO_REUSEPORT, group commit, bg ckpt |
| java-nio-stohirov (#88) | 102,000 | 61 | 555 | FFM epoll + FFM SQLite, LRU RST eviction |
| node-uwebsockets (#102) | 97,000 | 90 | 336 | SQL-built JSON, Connection: close at fd limit |
| typescript-bun (#3 anchor) | 90,000 | 87 | 301 | LRU RST eviction, backlog via 2nd listen() |
| scala-netty-jacksonhoran (#107) | 90,000 | 92 | 825 | Netty 1 thread, FFM SQLite, LRU idle close |
| python-pypy-epoll (#84) | 90,000 | 94 | 270 | |
| fsharp-oxpecker (#108) | 65,000 | 80 | 1,255 | Kestrel; crashes at fd limit; bg ckpt thread |
| haskell-warp (#104) | 55,000 | 107 | 1,587 | idiomatic FP framework; memory-heavy |
| java-quarkus (#67) | 29,000 | 164 | 542 | **disk-bound: forgot synchronous=NORMAL** |

Closest idiomatic JVM data points from open PRs (local/DO numbers, not batch 2):
- java-micronaut-Clickin https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/116 : stock Micronaut +
  sqlite-jdbc, **84,500 VUs on a real DO droplet at 103 µs/req** (with LimitNOFILE raised to 131072, so not comparable).
- kotlin-springboot-Mindstorm1750 https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/66 : "conventional
  Spring" (Tomcat + virtual threads + JdbcClient + Hikari), 184 µs/req locally; best documented A/B table of JVM knobs.
- java-springboot-reactor-mohkamfer https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/138 : step-by-step
  CPU table, WebFlux 139 µs → raw Reactor Netty + FFM 82 µs.
- typescript-effect-SeeThruHead https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/49 : the closest
  philosophical twin (Effect-TS, typed errors, Schema everywhere, deliberately no SQL-JSON / raw sockets).

## 1. Batch 2 "Rule flags from code review" (avoid these)

| Flag | Entries | Our status |
|---|---|---|
| Tokens without `exp` accepted | asm-epoll #117, java-nio #88, quarkus #67, kotlin #83, scala-netty #107 (fixed in https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/149), node-uws #102, dart #46, go-champ3oy #76, pypy #84, csharp #74, bash #89 | OK: `Jwt.scala` requires `exp` (`get[Long]("exp")` must succeed). Keep a test for a no-`exp` token. |
| Reads see uncommitted writes in the same batch | Deno #119, Zig #125 | Risk if we add group commit and let reads join the write txn. Keep reads off the write txn, or hold their responses until COMMIT. |
| start.sh restart loops | Deno #119, c-epoll #64 | OK: `exec java`. |
| Unchecksummed toolchain downloads | Deno, Go #72, Haskell, Swift, .NET, rustup, Zig | OK: JDK + sbt checksum-verified. Keep it so if we switch to GraalVM JDK. sbt dependency fetch is pinned by version (fine). |
| SQL by string concat | bash #89 | OK: doobie fragments. |
| README ≠ code | c-iouring #112 | Keep README in sync. |
| Crash at fd limit (EMFILE) | F# #108, C# #74 | Relevant only if we go direct. Ember: check accept-loop behaviour on EMFILE (must not spin or die). |
| `synchronous` left at FULL by default | Quarkus #67 (disk-bound, 26-39% iowait) | OK: we set NORMAL. |

Rule-risk notes found while reading (not in batch-2 flags):
- go-epoll-MegaPanchamZ formats `created_at` in C and binds it instead of the column default (rule 7/3 grey zone). Avoid.
- c-iouring #112 sheds load from `/proc/meminfo` (allowed, but reads like benchmark tuning).
- "Prewarm by reading the DB file / running each statement once before binding" (bogdan-blaj #124) is allowed (page
  cache), not a result cache.
- Sending `Content-Encoding: identity` to dodge Nginx gzip would be rule-7 grey (it only exists to defeat the bench
  Nginx config). Avoid.

## 2. Techniques, grouped

Verdict key: **ADOPT** = adopt idiomatically; **MAYBE**; **AVOID** = too gamed / not idiomatic for a Typelevel app.

### 2.1 Connection handling

| Technique | Who | What it buys | Verdict for us |
|---|---|---|---|
| Direct on :80 instead of Nginx | every entry above 71k; all top JVM entries | Saves Nginx's 14-20% CPU; removes 16,384-conn cap; avoids Nginx gzip of feed | **MAYBE (big)**. Blocked by Ember's ~35 KB heap/conn. Netty-based JVM entries report ~3.8-4 KB/conn (ktor-netty https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/45 , micronaut #116). Idiomatic route: keep http4s routes, swap backend to **http4s-netty** (`org.http4s::http4s-netty-server`) and measure heap/conn. Ember direct is only viable to ~20-25k users. |
| LRU idle eviction near fd limit (close longest-idle with nothing in flight) | go #72, java-nio #88 (within 128 fds of limit, close 64, RST), scala-netty #107 (limit-1024, idle > 1 s), ktor #45, bun #3 (>64k close down to 63k, RST), virtualthreads https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/85 | Lets a 65,535-fd process "hold" 90-120k users (clients reconnect). Without it: stuck at 71,000ᶠ (batch-2 footnote) | **MAYBE**, only if direct. Not expressible in Ember's public API; would need a backend hook (Netty `ChannelGroup` + idle tracking). Only matters past ~60k users, which our heap won't reach soon. |
| `Connection: close` above `limit-1024` | node-uws #102, Deno redemption https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/150 , go #72 (soft tier) | Same goal, simpler; Deno went from 71k fd-capped to passing 72k on a slower droplet | **ADOPT if direct**: an http4s middleware that reads an open-connection counter (`Ref`) and adds `Connection: close`; Ember honours it and closes after the response. ~20 lines, idiomatic. |
| RST (`SO_LINGER=0`) on eviction | java-nio #88, bun #3, #85 | No TIME_WAIT / FIN exchange | AVOID for us (needs raw socket access). |
| Two processes with `SO_REUSEPORT` to double fds | kotlin-nio #83 (120k, but 1,380 MB RAM; WAL can't reset above ~100k because two processes share it) | 2× fd ceiling | AVOID: two JVMs on 2 GB, not idiomatic. |
| io_uring fixed-file tables to bypass RLIMIT_NOFILE | rust #82, asm #86, c-iouring #112 | 264k users | N/A on JVM. |
| Listen backlog 4096 via a 2nd `listen()` | bun #3 (Bun's fixed 512), java-nio #88, micronaut/netty `SO_BACKLOG` | Fewer dropped SYNs on reconnect bursts | Already done (FFM `listen()`). Keep. With http4s-netty it becomes a plain `ChannelOption.SO_BACKLOG`. |
| Keep-alive ≥ 65-75 s | uWS patched 10 s → 75 s (#102: 0.15% errors and p99 3.67 s → 0 errors, 28 ms) | Avoids close/reuse races | Already `idleTimeout = Inf`. Keep; never set a short idle timeout (k6 reuse races = errors). |
| Batch flush per read-complete | netty-itsnaveenk https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/57 , lfeagan https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/63 | Fewer syscalls with pipelining | N/A (k6 doesn't pipeline). |
| Adaptive wake-up batching (park 2 ms if < 32 events) | java-nio #88 (~10% CPU) | Amortizes epoll wakeups | AVOID (runtime-level; CE's scheduler owns this). |

### 2.2 SQLite tuning (pragmas, compile options)

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| WAL + `synchronous=NORMAL` | everyone sane | Quarkus #67 without it: 29k, disk-bound | Done. |
| `locking_mode=EXCLUSIVE` | node #102 (+5% mixed), bun, java-nio, go, c-iouring, rust #82, trikeshed https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/128 , flash https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/133 | WAL index in heap, no `-shm`, no fcntl per txn | **MAYBE**: requires **one connection for everything** (see 2.6). Conflicts with a background checkpoint connection (node #102 rejected bg ckpt for this reason). |
| `wal_autocheckpoint=0` + checkpoint off the request path | kotlin #83 (daemon thread, PASSIVE every 1 s), lfeagan #63 (same), F# #108 (thread every 100 ms after writes; RESTART under write lock at 65,536 frames), java-nio #88 (between batches, ~1/s), rust #141 https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/141 (bg PASSIVE every 4,096 frames, defer writes ≤500 ms at 65,536 frames) | F#: on a slow-fsync disk, p95 500 ms → 0.21 ms. A checkpoint does 2 fsyncs; inline it stalls whoever triggered it | **ADOPT**. For us it matters more than for others: our JDBC calls run as `delay` **on the single CE worker**, so the write that trips the default 1,000-page autocheckpoint blocks *all* fibers during its fsyncs. Sketch below. |
| Bigger `wal_autocheckpoint` (4000-20000) | node #102 (4000: +8% on write-heavy), netty-itsnaveenk #57 (20000) | Fewer checkpoints | Fallback if we don't want a fiber. Still inline on the compute thread. |
| Caveat: PASSIVE bg checkpoints may never catch up → unbounded WAL | ktor #45 (1.4 GB/min at saturation), node #102 (245 MB in 10 s), F# #108 (WAL 271 MB) | | Bound it: when `wal_checkpoint` reports log ≥ N frames, run `RESTART`/`TRUNCATE` **while holding the writer Mutex** (F# approach). Plus `journal_size_limit=64MB` (java-nio, kotlin). |
| `mmap_size` 1 GiB vs `mmap_size=0` + big page cache | mmap: most; no mmap: go #72 (pcache1 256 MiB, claims WAL-mode mmap path skips pcache), ktor #45 (remap after checkpoint growth cost ~20%), quarkus-panama | Mixed evidence | **Keep mmap** for us. Important: with several connections, **a write by another connection drops each reader's whole page cache** (ktor #45; PR #66: "with mmap, re-reading pages after another connection's write is cheap"). With our read pool, mmap is what makes that cheap. Revisit only if we go single-connection. |
| `cache_size` | 500 pages (rust, bogdan, flash) … 64-256 MiB (others) | | Fine as is (16 MiB/conn). |
| `temp_store=MEMORY` | many | Feed `ORDER BY` uses index, so ~nothing | Done; harmless. |
| `query_only=1` on reader connections | spring #33, netty #57, #85, reactor #138 | Safety, ~no perf | **ADOPT** (cheap, documents intent): `config.addDataSourceProperty("query_only","true")` on the reader pool only... sqlite-jdbc may not map it; else `connectionInitSql = "PRAGMA query_only=1"`. |
| sqlite-jdbc `jdbc.get_generated_keys=false` | spring #33, #66, ktor #45 ("prepares `SELECT last_insert_rowid()` after every INSERT") | Verified in sqlite-jdbc 3.53.4.0 bytecode: `CoreStatement.updateGeneratedKeys()` runs `createStatement().executeQuery("SELECT last_insert_rowid();")` after any INSERT run via `executeUpdate`/`execute` when the flag is on (default). **Our like (`.update.run`) pays an extra prepare+step per like.** | **ADOPT**: `config.addDataSourceProperty("jdbc.get_generated_keys", "false")` on both pools. One line. |
| Compile SQLite ourselves (`THREADSAFE=0/2`, `DEFAULT_MEMSTATUS=0`, `OMIT_*`, `-O2 -march=native`, PGO) | all FFM entries; PGO: rust, go, asm, c | THREADSAFE/MEMSTATUS remove mutexes per call; reactor #138: FFM + options 91 → 82 µs | **AVOID** (means FFM bindings instead of sqlite-jdbc; leaves doobie). sqlite-jdbc's bundled build is fine. |
| `SQLITE_ENABLE_QPSG` | java-nio #88, trikeshed #128 ("re-prepare of post-by-id on every new bound id took 17% of CPU" on 3.54.0), v #113, nim | Prevents re-prepare per bind | Not available in sqlite-jdbc's build. Irrelevant today (we prepare per request), **but it caps the gain of statement caching for `/posts/:id`**. Measure; a different SQL shape for the by-id query may avoid the flag that forces re-prepare. |

### 2.3 Query shapes

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| Reference feed query (20 correlated `count(*)`) | most | kotlin #83 JFR: ~59% of busy time in `sqlite3_step`, mostly the feed's 20 counts | Baseline. |
| "3 range scans" feed: 20 newest ids → if span < 256, one `likes WHERE post_id BETWEEN` scan + one `posts JOIN users WHERE id BETWEEN` scan → reorder; else fall back | rust #82, go #72, c-iouring #112, asm (opt-in) | rust: feed +23% (with read-txn change) | **MAYBE (low for us)**. Rule-OK (live queries). Costs 3 statements and Scala reassembly; SQLite feed time is maybe 15-25 µs of our ~260 µs. Do it only after statement caching, and only if a profile says the feed's counts dominate. Doobie: `ids <- q1.to[Vector]`, then `q2`, `q3`, `groupMapReduce`. |
| Like = `INSERT … SELECT … WHERE EXISTS … ON CONFLICT DO NOTHING`, existence probe only if 0 rows | scala-netty, ktor #45, lfeagan, reactor, #66, us | 1 statement on the common path | Done. |
| `INSERT … RETURNING id, created_at` (no extra SELECT) | #33, #66, us | | Done. |
| JSON built by SQLite (`json_object`, `group_concat`/`json_group_array`) | node #102 (+ dropping `ORDER BY` inside aggregate: 27 → 22 µs), kotlin #83, java-nio (per-row `json_object`, Java adds brackets: 13.9 → 9.7 µs), bogdan #124 (per-row cheaper than group_concat: 16 vs 22 µs), micronaut #116, spring #33 (Jackson was 12% of CPU) | Skips row mapping + JSON encoder | **AVOID / MAYBE-low**. F# #108 measured it slower (+10-31% CPU). Effect-TS #49 deliberately refused it. It moves the API shape into SQL; not idiomatic with circe. If ever tried: per-row `json_object` read as `String`, keep circe for the envelope. |

### 2.4 Write path

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| Group commit, batch = "whatever arrived in this loop turn", no timer | node #102 (+3%), kotlin #83, java-nio #88, lfeagan #63, micronaut #116 (≤256 drained), flash #133 (≤64), reactor #138 (≤256), bogdan #124, #85, Effect-TS #49 (`RequestResolver` batching) | Small: writes are ~8%. F# #108 measured ≤2% possible saving and found a writer-thread version cost 2× CPU per write due to hand-offs; #66 "not needed" | **MAYBE (low)**. Idiomatic, cheap to express, but expect ≤3%. Do NOT use a time window: node found a 5-20 ms window gave no CPU gain, only latency; trikeshed #128's 64 ms window did cut µs/req (67 → 56) but only because reads joined the txn. Sketch below. |
| Reads share the batch's transaction (one read txn per batch) | rust #82, go #72, asm #86, c-iouring #112, bogdan #124 | Saves per-statement read-txn setup | AVOID: needs single connection + hold read responses until COMMIT; Deno/Zig were flagged for getting this wrong. |
| `synchronous=FULL` + group commit to make it cheap | quarkus-panama https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/73 (2 ms window, one fsync/batch) | Real durability | Not needed (NORMAL allowed). |
| Single writer connection, writes queue in-process, not in SQLite busy handler | #33, #66, #57, reactor, us | No SQLITE_BUSY sleeps | Done (Mutex). |
| `busy_timeout=0` + retry from the loop | kotlin #83 (2 processes) | Avoids busy-handler sleep freezing the loop (0.5-1.4 s) | N/A with one process + Mutex. Keep `busy_timeout` > 0 as safety (bg checkpointer). |

### 2.5 Prepared-statement reuse

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| Prepare once, reuse (rule 5 explicitly allows) | every fast entry; `SQLITE_PREPARE_PERSISTENT` (java-nio, trikeshed) | #66: prepare ≈ 40% of SQLite time; with filter removal 227 → 204 µs; our profile: prepare ≈ 15% of app CPU | **ADOPT (highest DB-side win)**. Best documented JVM precedent: `StatementCachingDataSource` under Hikari in #66 (per-connection LRU, never evicts an in-use statement, no read snapshot held between uses). Sketch below. |

### 2.6 Threading model on 1 vCPU

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| One thread does everything (event loop runs SQLite inline) | all top JVM entries (scala-netty, java-nio, kotlin, ktor #45, lfeagan, bogdan, trikeshed) | No hand-offs. ktor #45: 2 threads ⇒ every like reset the other connection's page cache | Done in spirit: 1 CE worker, SQLite as `delay`. |
| **Single SQLite connection for reads and writes** | ktor #45, java-nio, scala-netty, kotlin, node, bun, micronaut (one `db-owner` thread), flash | No cache invalidation across connections, enables `EXCLUSIVE`, statement cache lives in one place | **MAYBE (medium)**. With one worker thread and no async boundary inside a DB call, a read pool buys no parallelism. A `Mutex`-guarded single `Transactor` (pool size 1) is just as idiomatic. Cost: the checkpoint must run on that connection (under the Mutex, as `IO.blocking`), so requests wait during fsyncs; node #102 chose this and it won. Measure vs. current. |
| Virtual threads | #66 (beat 16-thread pool by 29% CPU), #85, agrison #101 | | N/A (CE fibers). |
| Dedicated DB thread + queue | micronaut #116, flash #133, reactor #138 writer, vertx https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/92 | | AVOID: hand-off per request (F# measured 2× CPU per write). |

### 2.7 HTTP layer, JSON, JWT

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| Hand-written HTTP parser / raw Netty / FFM epoll | all top JVM | 2-3× less CPU | AVOID (that's replacing http4s). |
| Drop framework layers you don't use | reactor #138 (WebFlux → raw route: 139 → 106 µs), #66 (filters off + stmt cache: 227 → 204 µs; no kotlin-reflect −5%), micronaut (build-time serde), ktor #45 (no plugins, leak detector off) | | **ADOPT where possible**: keep the `HttpApp` minimal (no logger/CORS/GZip middleware, `Router` over plain pattern match is fine); check Ember's per-request header validation/`Date` header cost in a profile. |
| circe: encode straight to bytes | — | Avoid `Json` AST + `String` | **MAYBE**: give `Post` an `Encoder.AsObject` (done) and emit with `Printer.noSpaces.printToByteBuffer` / `EntityEncoder` via `jsonEncoderOf` with a byte printer; or `jsoniter-scala` codecs (still idiomatic Scala, much faster). jsoniter-scala is the idiomatic high-perf choice; circe is the Typelevel one. |
| JWT: reuse keyed `Mac` (clone/ThreadLocal), constant-time compare, precomputed HMAC pads (`MessageDigest` instead of `Mac.doFinal` which allocates) | all; node `KeyObject` (+5%), bogdan #124 pads, c-iouring pads | Cheap HMAC | Low impact for us (JWT only on ~8% of requests). We already clone a keyed `Mac`. Optional: avoid `s"$header.$payload"` and two circe parses (`Mac.update` on the raw token bytes up to the 2nd dot). **Never cache verification (rule 5).** |

### 2.8 JVM flags and runtime

| Technique | Who | What it buys | Verdict |
|---|---|---|---|
| Serial GC, fixed heap, `AlwaysPreTouch` | all JVM entries | ZGC: −p99 but +28% CPU (ktor #45); G1 vs Serial at 56k VUs: 3.0 s vs 2.1 s pause (micronaut) | Done. |
| Fixed young gen + `MaxTenuringThreshold` small | micronaut (`-Xmn384m -XX:MaxTenuringThreshold=3`: avoided 936 ms Full GC), scala-netty (promote after 2) | Long-lived conn objects stop being copied | **ADOPT** if direct (many long-lived conns). Behind Nginx: already tuned. |
| `-XX:+UseCompactObjectHeaders` (JDK 25 product flag) | java-nio, kotlin, ktor #45, lfeagan, bogdan | Less heap per object (helps per-connection heap); #66: no CPU change | **ADOPT**: one flag, free memory, matters if we go direct. |
| `-XX:ActiveProcessorCount=1` | micronaut | Makes every library size its pools for 1 CPU | **ADOPT** (cheap safety; the box has 1 vCPU anyway). |
| Graal JIT (Oracle GraalVM JDK, JIT mode) | #66 (6-20% less CPU/request than C2 at 1 GB heap), trikeshed #128 | Better escape analysis/inlining; usually large for Scala/FP allocation-heavy code | **ADOPT-candidate (high)**: install Oracle GraalVM for JDK 25 tarball (checksum-pinned, GFTC license), same `start.sh`. Watch: JIT compile threads compete for the vCPU during each probe's ramp. |
| `-XX:+UseTransparentHugePages` | #66 (~2.5% CPU) | THP is `madvise` on Ubuntu, so this is in-process, not kernel tuning | **ADOPT** (one flag). |
| GraalVM native-image | ktor-graalvm https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/53 ; #66 tried | #66: can't build on the 2 GB box (needs ~1.8 GB), and +15% CPU/request vs Graal JIT without PGO | AVOID. |
| AOT cache / CDS / Leyden | ktor #36 https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/36 , trikeshed | Startup only | AVOID/low: startup isn't scored (60 s limit). JDK 25 AOT method profiles could shorten per-probe warm-up, but needs a training run at build time. |
| Pre-warm before binding (run each read statement once, touch the DB file) | bogdan #124 | Faster first minute of each probe (every probe restarts the app) | **MAYBE**: a `Resource` that runs `ping`, `feed`, `post(1)` N times through the `HttpApp` (in-process `Request`s) before `EmberServerBuilder.build`. Reads only; no writes. |
| `-XX:-UsePerfData`, capped metaspace/direct memory | lfeagan, java-nio, us | Small | Done. |
| CE: 1 worker, tracing off, no starvation checker | us | | Done. Optional: raise `autoYieldThreshold` (fewer auto-cedes); measure. |

## 3. Sketches for the adoptable ones (Cats Effect / fs2 / doobie)

### 3.1 Statement cache inside doobie (rule 5: plans only)

Doobie's extension point is the `KleisliInterpreter` we already override. Wrap the JDBC `Connection` once per pooled
connection so that `prepareStatement(sql)` returns a cached `PreparedStatement` whose `close()` only does
`clearParameters()` (doobie closes statements in `Resource` finalizers).

```scala
final class CachingConnection(underlying: java.sql.Connection) extends ConnectionDelegate(underlying):
  private val cache = java.util.HashMap[String, java.sql.PreparedStatement]()
  override def prepareStatement(sql: String): java.sql.PreparedStatement =
    cache.computeIfAbsent(sql, s => ReusableStatement(underlying.prepareStatement(s)))
// ReusableStatement: delegate everything; close() = clearParameters(); realClose() on connection close.
```

Plug it in at the `Transactor` connect step: `Resource.make(IO.delay(CachingConnection.of(ds.getConnection)))(...)`
keyed on the physical connection (Hikari reuses it), or as a Hikari `DataSource` wrapper like #66. Every fragment in
`Repository` is a constant SQL string, so the cache is bounded (6 statements). Caveat: ResultSets must be closed
before reuse (doobie does). Caveat: no QPSG in sqlite-jdbc's SQLite → the by-id statement may still re-prepare per
bind; profile it.

### 3.2 Background checkpointer (supervised fiber)

```scala
// writer connection: PRAGMA wal_autocheckpoint=0 (data-source property "wal_autocheckpoint" or connectionInitSql)
val checkpointer: Resource[IO, Unit] =
  Stream.fixedDelay[IO](1.second)
    .evalMap(_ => db.checkpoint("PASSIVE"))          // own 1-conn transactor; run as IO.blocking (fsync!)
    .evalMap { case (busy, log, done) =>
      IO.whenA(log - done > 16384)(db.writeLock.lock.surround(db.checkpoint("RESTART"))) }
    .compile.drain.background.void
```

`PRAGMA wal_checkpoint(PASSIVE)` returns `(busy, log, checkpointed)`. Run it with `IO.blocking` (or `interruptible`) so
the fsyncs happen on the blocking pool, not the single compute worker. Add `journal_size_limit=67108864`.
Not compatible with `locking_mode=EXCLUSIVE` (needs a 2nd connection).

### 3.3 Opportunistic group commit

```scala
final case class WriteReq[A](op: ConnectionIO[A], done: Deferred[IO, Either[Throwable, A]])
queue <- Queue.unbounded[IO, WriteReq[?]]
Stream.fromQueueUnterminated(queue)              // emits whatever is already queued as one chunk
  .chunks                                         // no timer: batch = what arrived meanwhile
  .evalMap { batch =>
    val txn = batch.traverse(r => savepointed(r.op).map(r -> _))  // per-request savepoint
    txn.transact(writerWithStrategyDefault).attempt.flatMap {
      case Right(rs) => rs.traverse_((r, res) => r.done.complete(res))   // only after COMMIT
      case Left(e)   => batch.traverse_(_.done.complete(Left(e)))        // whole batch → 500
    }
  }.compile.drain.background
```

Reads stay on the reader connections (no uncommitted reads). Expected gain ≤3% (writes are 8%).

### 3.4 `Connection: close` under fd pressure (only if direct)

`HttpApp` middleware: `Kleisli(req => app(req).flatMap(resp => openConns.get.map(n => if n > limit - 1024 then
resp.putHeaders(Connection.close) else resp)))`. The open-connection count needs a backend hook (Ember: none public;
http4s-netty: a `ChannelInboundHandler` counting active channels). Read the limit from `/proc/self/limits` or
`ulimit -n` in `start.sh`.

## 4. Ranked recommendations for our idiomatic server

Ranked by expected impact (my estimate, confidence in brackets):

1. **Statement cache in the doobie interpreter** (−10-15% CPU; prepare ≈15% of our CPU, #66 got 227 → 204 µs) [0.8]
2. **Graal JIT (Oracle GraalVM JDK 25), + THP, + compact headers, + ActiveProcessorCount=1** (−5-20% CPU, from #66) [0.6]
3. **Background checkpointer fiber with `wal_autocheckpoint=0`, fsync on the blocking pool** (tail latency; removes fsync stalls of the only worker thread; F#: p95 500 → 0.21 ms on slow disk) [0.7]
4. **Go direct via http4s-netty** instead of Ember+Nginx (removes Nginx's 14-20% CPU, Nginx gzip of every feed and the 16k cap; needs ~4-6 KB/conn instead of 35 KB) [0.5: per-conn heap of http4s-netty unmeasured]
5. **Single SQLite connection behind the Mutex (+ `locking_mode=EXCLUSIVE`)** (node: +5%; no cross-connection cache resets) [0.5; conflicts with #3, pick by measurement]
6. **`jdbc.get_generated_keys=false`** (removes an extra prepare+query per like; ~1%, one line) [0.9 that it works, small gain]
7. **Faster JSON output** (jsoniter-scala or circe printing straight to bytes; spring #33: Jackson was 12% of CPU) [0.5]
8. **Opportunistic group commit via fs2 `Queue` + `chunks` + `Deferred`** (≤3%) [0.6]; then range-scan feed only if profiles show the 20 counts dominate.

Avoid:
1. FFM SQLite / custom-compiled SQLite / hand-written HTTP / SQL-built JSON: fast (top JVM entries) but replaces the Typelevel stack.
2. Reads joining the write transaction, time-window batching, or own `created_at` timestamps (rule-6/7 flags; Deno/Zig flagged).
3. Native-image (can't build on the box; slower than Graal JIT), two-JVM `SO_REUSEPORT`, and gzip-dodging headers.

## The challenge owner's videos

Arjay McCandless ([YouTube](https://www.youtube.com/channel/UCdSsl5RM4Tne4MhDRz2HjWw)) runs both challenges. His
videos on them, transcribed and read for this submission (one transcript from YouTube's captions, the rest from the
audio with Whisper `small.en`):

| Date | Video | What it is |
|---|---|---|
| 2026-09-25 | [How Many Users Can a $12 Server Handle?](https://www.youtube.com/watch?v=GFtFIxrAjIs) | Node + Postgres + Nginx on the $12 box; the limit is CPU; caching took it from 2,500 to 5,200 users |
| 2026-10-02 | [Which Programming Language Can Handle the Most Users on a $12 Server?](https://www.youtube.com/watch?v=sQXFhh_PiG4) | 8 stacks, one query per endpoint, no caching; Spring Boot 5,100 users on Postgres, 10,250 on SQLite; the $12 challenge's origin |
| 2026-10-09 | [I Ran 4 Databases on the Same $12 Server](https://www.youtube.com/watch?v=_M2-Dw4DpsE) | Postgres, MySQL, MariaDB, SQLite; durability: SQLite `synchronous=FULL` collapses on writes without group commit |
| 2026-10-13 | [I Asked Expert Programmers to Max Out a $12 Server](https://www.youtube.com/watch?v=QGWKCaglSBI) | Batch 1 results, the common tricks, and the trade-offs he wouldn't ship |
| 2026-10-16 | [More CPUs = Better? I tested 4 Programming Languages](https://www.youtube.com/watch?v=pG3xXuNdL34) | 1/2/4/8 vCPUs, Postgres; the multicore challenge's origin |
| 2026-10-23 | [I Asked Developers to Max Out a $12 Server (again)](https://www.youtube.com/watch?v=-BFt49ula08) | Batch 2 results, entry by entry, including the Scala/Netty one |

### What he measures and values

- **The limit is CPU** for this workload (his first video: 80% CPU at 2,500 users, 99% at 4,000; disk, RAM and network
  were fine; CPU steal 0-2% even on a shared droplet). Under a write-heavy load it moves to **disk**.
- **vCPUs are hyper-threads**, and per-core speed differs between machines sold under the same label (up to 20% in
  his tests); batch 2 moved to a different provider and scores doubled with the same code.
- His **load generators** became the limit twice (k6 memory, then source ports at ~240,000 connections per target IP);
  batch 2 used mload, replaying the k6 user model (one request per ~10 s per user).
- **The tricks the fastest entries shared** (batch 1): no Nginx, statements prepared once on one connection, JSON built
  by SQLite, group commit, frameworks replaced by hand-written servers. Batch 2 added io_uring with registered file
  tables (to get past the 65,535-descriptor limit), evicting idle connections near the limit, range-scan feeds, PGO.
- **What stopped entries** in batch 2: CPU, memory (Go net/http and Haskell were OOM-killed, the winner at 360k), the
  descriptor limit (four entries tied at 71,000), storage waits (Quarkus at `synchronous=FULL` with per-request commits:
  20-39% iowait; F#'s checkpointing), and a crash in an error handler (C#).
- **What he would not ship**: "I probably wouldn't ship most of these implementations or approve these PRs" (1,300-line
  single-file servers, unsafe Rust, partial HTTP), coupling to SQLite internals, and **durability**: "many of the
  fastest entries use SQLite's write-ahead log with synchronous set to normal ... a write could still be lost if the
  machine loses power ... a trade-off I would not use in production". His database video shows the alternative: the
  server databases fsync every commit and use group commit to share the flush; SQLite at `FULL` without it fell from
  6,800 to 1,550 users on a write-heavy load.
- **Design details he called out as good**: a busy SQLite call must not put an event loop to sleep (Kotlin's
  zero-busy-timeout writer), idle timeouts too short turn keep-alive into reconnect churn (Node), reads on a pool and
  writes through one serial writer (Swift, Go), statements prepared once, constant error responses built ahead (Swift),
  a background checkpointer must be able to finish or the WAL grows (assembly, F#).
- **The Scala entry** (Netty, one event loop, FFM SQLite, idle eviction): 90,000 users on the batch-2 machine, p95 484 ms.
