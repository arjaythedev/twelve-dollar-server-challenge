# Java + Netty

| | |
|---|---|
| Language | Java 21 LTS (Ubuntu 24.04's `openjdk-21-jdk-headless` package) |
| Framework | Raw Netty 4.1.118.Final (`netty-all` with native epoll transport) |
| Server | Netty `EpollServerSocketChannel` (Linux edge-triggered native epoll) |
| SQLite driver | `org.xerial:sqlite-jdbc` 3.45.1.0 linked to SQLite 3.45.1 (WAL mode + mmap) |
| JSON | Custom zero-allocation streaming `ByteBuf` byte serializer (`FastJsonWriter`) |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself |

All dependencies are pinned in `pom.xml`.

## Running it

```bash
sudo bash install.sh   # apt: openjdk-21-jdk-headless, maven
bash build.sh          # packages fat JAR via maven-shade-plugin
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **Direct instead of Nginx.** On a 1 vCPU machine, Nginx consumes 15–20% of the entire CPU budget just proxying bytes. Direct mode gives 100% of the CPU core to Java. Furthermore, Nginx sets `worker_connections 16384` in the benchmark harness (which counts both client and upstream sockets), placing a hard ceiling at ~16k concurrent users. In direct mode, our Netty server leverages the droplet's `LimitNOFILE=65535` and handles 60,000+ open keep-alive connections directly.
- **Direct ResultSet-to-ByteBuf Streaming.** On `/feed` requests (46% of traffic), standard JDBC maps rows to domain records (`List<Post>`), allocating over 80 objects per request. At 3,000 req/s, that produces >240,000 short-lived objects per second. `Database.writeFeedJson()` and `writePostByIdJson()` stream row data directly from the native SQLite `ResultSet` into Netty's pre-sized `ByteBuf`, completely eliminating domain record and list allocations.
- **Batched Socket Flushes via `channelReadComplete()`.** Instead of issuing an immediate `writeAndFlush` system call on every single request, responses are queued with `ctx.write()` and flushed together during Netty's `channelReadComplete(ctx)` hook. This coalesces multiple pipelined or batched responses into a single `writev()` system call, drastically reducing OS syscall overhead.
- **Single Netty Epoll EventLoop with Inline Writes.** Since the droplet has only 1 vCPU, multi-threaded worker pools or background write threads force costly OS thread context switches (20–50 $\mu s$ per switch) and synchronization locks. In SQLite WAL mode with `PRAGMA synchronous = NORMAL`, commits do not fsync to disk and take under 35 $\mu s$. Running writes inline directly on the event loop eliminates thread hops, queue contention, and future allocations entirely.
- **Dedicated Read vs. Write Connections in SQLite WAL.** In SQLite WAL mode, readers do not block writers and writers do not block readers, *provided they use separate connection handles*. We maintain an independent `connRead` (with `PRAGMA query_only = 1;`) for non-blocking `/feed` and `/posts/:id` queries, and a dedicated `connWrite` for inserts.
- **Zero-Allocation Custom JSON Byte Serializer (`FastJsonWriter`).** General-purpose reflection mappers (Jackson, Gson) allocate dozens of DTOs, Maps, and Strings per request. `FastJsonWriter` streams ASCII tokens, UTF-8 strings, and integers directly into Netty's pooled `ByteBuf` with zero heap object allocations:
  - Long digits are formatted using a thread-local scratch buffer rather than allocating `byte[]` arrays.
  - Strings are written using Netty's 4-argument `ByteBufUtil.writeUtf8(buf, str, start, end)` to prevent `subSequence` substring allocations.
  - ISO-8601 timestamps and usernames are written directly as ASCII without escape scanning.
- **Zero-Copy Path and ID Parsing.** `StringUtil.parsePositiveId(CharSequence seq, int start, int end)` parses route parameters directly from the URI string into primitive `long`s without substring allocations or `java.lang.Long` heap boxing.
- **Sub-10 $\mu s$ Custom JWT Verification.** Rule 5 forbids caching JWT verifications, requiring every authenticated request to re-verify the HMAC signature. We use `javax.crypto.Mac` pre-initialized with the challenge secret and reused per thread via `ThreadLocal` (`mac.reset()`). Bytes are copied directly into a thread-local ASCII buffer, avoiding String conversions. Common standard headers (`{"alg":"HS256","typ":"JWT"}`) are fast-path matched to bypass base64 decoding.
- **Kernel Socket Options & Buffer Tuning:**
  - `SO_RCVBUF = 4096`
  - `SO_SNDBUF = 16384` (ensures full feed responses fit in a single TCP send window without epoll wait cycles)
  - `SO_KEEPALIVE = true`
  - `TCP_NODELAY = true`
  - `TCP_QUICKACK = true` (disables delayed ACKs to minimize round-trip latency)
  - `SO_BACKLOG = 8192`
- **SQLite PRAGMAs:**
  - `journal_mode = WAL`: non-blocking append-only logging.
  - `synchronous = NORMAL`: satisfies Rule 6 durability without full fsync pauses.
  - `mmap_size = 1073741824` (1 GiB): reads directly access OS page cache pages without copying.
  - `cache_size = -65536` (64 MiB page cache).
  - `temp_store = MEMORY`.
  - `threads = 0`: prevents SQLite from spawning helper threads on the single core.
  - `wal_autocheckpoint = 20000`: prevents mid-benchmark WAL checkpoint stalls.
- **Serial Garbage Collector & JVM Tuning.** Configured with `-XX:+UseSerialGC -Xms512m -Xmx512m -Xmn384m -XX:+AlwaysPreTouch` to eliminate background GC threads, fix heap boundaries, pre-fault memory pages, and keep young gen collections virtually non-existent. Netty bounds checks and accessibility validations in hot loops are disabled via `-Dio.netty.buffer.checkBounds=false -Dio.netty.buffer.checkAccessible=false`.

## License

MIT, under the repo's [license](../../LICENSE).
