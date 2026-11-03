# Java 25 + Micronaut + SQLite

| | |
| --- | --- |
| Language | Java 25 (Eclipse Temurin `25.0.2+10`, SHA-256 verified by `install.sh`) |
| Framework | Micronaut 5.2.15 (Platform 5.2.2) on Netty 4.2.19.Final (`nio`) |
| JSON | Micronaut Serialization (Serde) 3.2.4 with **build-time bean introspection**: `@Serdeable` records are processed by `micronaut-serde-processor` at compile time, so there is no runtime reflection and no `jackson-databind`. Jackson annotations (`@JsonPropertyOrder`) fix the field order, and only the Jackson 3.2.3 streaming core reads and writes the JSON text. It handles request bodies and the small write, health, and error responses; feed and post reads are JSON bytes rendered by SQLite and bypass it. |
| JWT | Hand-written HS256 check on the JDK's `Mac` (HmacSHA256): verified on every authenticated request (`POST /posts`, `POST /posts/:id/like`); `alg` must be `HS256`, constant-time signature comparison (`MessageDigest.isEqual`), fresh `exp`, `sub`, and `username` checks. No verification cache. |
| SQLite driver | `org.xerial:sqlite-jdbc` 3.53.4.0 |
| Build | Gradle 9.5.0 wrapper (checksum-verified), Shadow 9.6.1, dependencies pinned in `gradle.lockfile` |
| **Nginx or direct** | **Direct** on `HOST:PORT` (`0.0.0.0:80` on the benchmark VM, default `127.0.0.1:3000`) |

## Run

```sh
sudo bash submissions/java-micronaut-Clickin/install.sh
bash seed/make-seed.sh
bash test/run.sh submissions/java-micronaut-Clickin
```

`build.sh` runs the JUnit suite and builds `build/libs/twelve-dollar-micronaut.jar`. `start.sh` runs that JAR in the foreground using `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT`. Startup fails if `$SQLITE_PATH` is missing; the server never creates or alters the schema.

## Design

This submission stays inside standard Micronaut and `sqlite-jdbc`—no custom HTTP parser, raw socket server, or FFM binding—and removes per-request work on one vCPU:

- **Single NIO event loop + single DB owner.** One Netty `nio` event loop handles HTTP parsing, Micronaut routing, JWT verification, and input validation. One dedicated `db-owner` thread owns a single SQLite connection with all statements (`SELECT 1`, feed, post, create, like, post-exists, `BEGIN IMMEDIATE`, `COMMIT`, `ROLLBACK`) prepared at startup. JDBC never blocks the event loop.
- **Opportunistic group commit.** `db-owner` blocks on a bounded 4,096-slot queue and drains up to 256 already-waiting operations per pass, with no batch timer or minimum batch size. If a batch contains any write, the whole batch runs inside `BEGIN IMMEDIATE … COMMIT`. Every response in the batch—including reads that observed uncommitted writes—is completed only after `COMMIT` succeeds. Unexpected SQL or commit failures roll back and fail the batch with 500; a full queue returns 503.
- **SQLite-rendered JSON bytes.** Feed and single-post reads return a single UTF-8 `BLOB` built by SQLite (`json_object` and `group_concat(... ORDER BY created_at DESC, id DESC)`). `SqlJsonBodyWriter` passes that `byte[]` directly to Micronaut's `ByteBodyFactory`, skipping Java row mapping and Jackson re-serialization on the read path (92% of traffic). Likes use `INSERT INTO likes ... SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING` and only query `posts` when zero rows are inserted to distinguish duplicates (`200`) from missing posts (`404`).
- **Per-request HS256 verification.** Every authenticated request verifies the signature with a thread-local pre-keyed `HmacSHA256` `Mac`, constant-time digest comparison (`MessageDigest.isEqual`), and fresh `alg`, `exp`, `sub`, and `username` checks. Verified tokens and query results are never cached.
- **SQLite pragmas.** `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=1000`, `cache_size=-8192` (8 MiB), and `mmap_size=268435456` (256 MiB).

### JVM & GC (`start.sh`)

```
-XX:ActiveProcessorCount=1 -XX:+UseSerialGC
-Xms768m -Xmx768m -Xmn384m -XX:MaxTenuringThreshold=3
-XX:MetaspaceSize=64m -XX:MaxMetaspaceSize=64m -XX:+AlwaysPreTouch
```

At 56k–64k open keep-alive connections, live old-generation footprint is ~220–250 MB (~4 KB per connection) and metaspace is ~31 MB:

- **Serial GC over G1.** On 1 vCPU there is no spare core for concurrent marking threads. At 56,000 VUs on the same 768 MB / 384 MB heap, G1 spent 3.0–3.3 s in GC pauses (max 181–197 ms) over a 5-minute hold versus 2.1–2.3 s (max 85–100 ms) with Serial GC.
- **Fixed 384 MB young generation (`-Xmn384m`) and `-XX:MaxTenuringThreshold=3`.** With a dynamic `-Xms128m -Xmx512m` heap, `DefNew` stayed near its initial 38 MB, overflowed survivor spaces during the ramp, promoted ~99 MB of short-lived objects into the 341 MB old generation, and triggered a ~936 ms Full GC at 56,000 VUs. Fixing the young generation at 384 MB with tenuring threshold 3 keeps steady-state holds in minor collections only and raised the passing level on `syd1` from 54,000 to 62,500 VUs.

## Measurements (`syd1`, `s-1vcpu-2gb`)

**Setup.** Server: DigitalOcean `s-1vcpu-2gb` (1 vCPU, 2 GB RAM, no swap, Ubuntu 24.04, Direct on port 80). Load: two 4 vCPU droplets in the same `syd1` VPC, each running an `rlt`-based Rust implementation of `bench/load.js` (verified against k6 1.3.0 for identical request mix, think times, headers, and server-side CPU/req; the ramp follows k6's `ramping-vus` activation schedule). This avoids k6's memory use at tens of thousands of VUs, and a single source IP can hold only about 64.5k connections to one destination port, so the VUs are split in half across the two generators.

**Procedure** (the README's scoring procedure): one server process for the whole search, a 1,000-VU 2-minute warm-up that is not scored, then 2,500 VUs doubling until a run fails, narrowing to within 250 VUs, and a confirmation hold at the best passing level (stepping down by 250 on failure). Every run is the official shape: 60 s ramp, 5-minute hold, 30 s ramp-down. Pass: p95 < 500 ms, p99 < 1 s, errors < 1% (a run fails if either generator fails). Hold columns show the worse of the two generators.

**Confirmed maximum: 84,500 VUs** (hold p99 218 ms, p95 98 ms, 0% errors, 92% CPU, 103 µs per request). 84,750 VUs failed (p99 1.9 s, 0.67% errors).

**Comfortable operating range: about 70,000 VUs.** The 80,000-VU doubling step already shows p99 903 ms with 0.05% errors, and between 82,500 and 84,750 VUs p99 swings from 217 ms to 1.9 s from run to run, so the confirmed maximum sits right at the edge. At 70,000 VUs (the same JAR, same droplets, two runs each of an A/B) CPU stayed at 86–87% and hold p99 was 134–216 ms with 0% errors in both runs.

| Step | VUs | Result | Hold req/s | Hold p95 (ms) | Hold p99 (ms) | Errors | Server CPU | CPU / req | Full GC |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| double | 2,500 | pass | 265 | 1 | 8 | 0.00% | 10% | 377 µs | 0 |
| double | 5,000 | pass | 529 | 1 | 8 | 0.00% | 15% | 280 µs | 0 |
| double | 10,000 | pass | 1,058 | 1 | 11 | 0.00% | 24% | 229 µs | 0 |
| double | 20,000 | pass | 2,118 | 1 | 14 | 0.00% | 39% | 186 µs | 0 |
| double | 40,000 | pass | 4,232 | 5 | 21 | 0.00% | 63% | 148 µs | 0 |
| double | 80,000 | pass | 8,423 | 173 | 903 | 0.05% | 91% | 108 µs | 1 |
| double | 100,000 | **fail** | 10,117 | 2877 | 3671 | 1.23% | 99% | 98 µs | 4 |
| narrow | 90,000 | **fail** | 9,392 | 690 | 1123 | 0.38% | 97% | 103 µs | 1 |
| narrow | 85,000 | **fail** | 8,875 | 962 | 1577 | 0.77% | 95% | 108 µs | 1 |
| narrow | 82,500 | pass | 8,689 | 247 | 819 | 0.21% | 92% | 106 µs | 1 |
| narrow | 83,750 | pass | 8,815 | 274 | 948 | 0.22% | 93% | 106 µs | 1 |
| narrow | 84,250 | pass | 8,897 | 107 | 316 | 0.03% | 94% | 105 µs | 1 |
| narrow | 84,500 | pass | 8,922 | 99 | 241 | 0.00% | 92% | 103 µs | 1 |
| narrow | 84,750 | **fail** | 8,882 | 824 | 1921 | 0.67% | 94% | 106 µs | 1 |
| confirm | 84,500 | pass | 8,922 | 97 | 217 | 0.00% | 92% | 103 µs | 1 |

Notes:

- **Raised descriptor limit.** This search ran the server with `LimitNOFILE=131072`, because more than 65,535 connections need more descriptors than the challenge's default per-process limit of 65,535 (it reached 70,018 open descriptors at 70,000 VUs). With the default limit, one process holds at most about 65.5k connections, so the score cannot exceed that regardless of CPU. On the default limit, with a single generator and a fresh JVM per step (an earlier revision of this code, before the single-`CompletableFuture` change, which measured the same CPU per request in an A/B), **64,000 VUs** passed (6,766 req/s, p95 48 ms, p99 283 ms, 0% errors, 82% CPU, 120 µs/req, 0 Full GCs). A 65,000-VU run failed on the generator's side (it stopped at 64,512 connections, the source-port limit), while the server peaked at 64,530 descriptors.
- **Near the limit it is flaky.** Between 80,000 and 84,750 VUs, hold p99 swings between 217 ms and 1.9 s, with the CPU at 91–94%. A single Full GC of about 0.4–0.7 s per run appears from 80,000 VUs up, because earlier runs' dead connection state remains in the old generation of the same JVM.
- **Host variance.** The same code cost 225 µs per request on an `sgp1` droplet and 103–150 µs on `syd1` droplets; steal spikes and host differences move the ceiling by several thousand VUs. Earlier searches on other `syd1` droplets confirmed 79,500 VUs (previous revision) and 62,500 VUs (a busier host pair), so treat the confirmed maximum as a range of roughly 80–85k on this hardware rather than a fixed number.
- **HotSpot JIT over GraalVM Native Image.** Native Image builds of the same JAR (Oracle GraalVM 25, `-O3`, Serial GC, with and without profile-guided optimization) were compared with HotSpot at 70,000 VUs, two runs each in ABBA order, same droplets: JIT cost 116–118 µs per request with hold p99 134–216 ms; native PGO 121–123 µs with p99 about 555 ms; native plain 126–130 µs with p99 about 553 ms in one run and 1.7 s (failed) in the other. JIT won on every metric, so `start.sh` runs the JAR on HotSpot.
- **Not an official score.** This is a self-run search with a Rust generator, not the k6 run on the maintainer's droplet.


## Verification

- **Official suite:** `test/run.sh submissions/java-micronaut-Clickin` passes **42/42**.
- **Unit & batch-durability tests (`build.sh`):** 8 JUnit tests in `BehaviorTest` and `DbExecutorTest` verify JWT edge cases, auth-before-validation ordering, Unicode length limits, post-commit read/write publication, batch rollback on statement or `COMMIT` failure, admission stop on lost transactions, 503 queue-overflow rejection, and graceful shutdown draining.
- **Supplemental HTTP & SQLite integrity checks:**

```sh
# Against a running server on a disposable seed copy:
python3 submissions/java-micronaut-Clickin/checks/verify-extra.py

# Standalone (starts its own server and temporary database):
python3 submissions/java-micronaut-Clickin/checks/verify-feed.py
```

Every benchmark and verification run also checked `PRAGMA integrity_check` (`ok`) and `PRAGMA foreign_key_check` (empty) after shutdown. `checks/mixed.lua` is included for local `wrk` A/B runs (100 feeds : 100 post reads : 15 likes : 2 creates).
