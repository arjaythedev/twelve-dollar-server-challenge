# C++23，无框架：io_uring + SQLite B-tree 读取

一个单线程的 HTTP 服务：五个接口，JSON 进 JSON 出。io_uring 事件循环、HTTP/1.1 解析、JSON 读写、HS256 校验和 SQLite B-tree 读取都在同一个线程里完成；SQLite 使用固定版本、未修改的 amalgamation，静态链接。唯一的例外是 WAL checkpoint，由一个额外的低优先级线程执行。

| | |
|---|---|
| 语言 | C++23，g++ 13（Ubuntu 24.04 `build-essential`） |
| 构建 | xmake（由 `install.sh` 安装），release 模式 + LTO |
| 依赖 | liburing 2.5（Ubuntu 24.04 `liburing-dev`）、SQLite 3.54.1 amalgamation（sha256 固定，`build.sh` 下载，本地编译） |
| Nginx 还是直连 | **直连**：自己监听 `HOST:PORT`（基准机上为 `0.0.0.0:80`） |
| 要求 | Linux 6.1+（io_uring 的 `DEFER_TASKRUN`、multishot receive、provided-buffer ring）。没有 epoll 回退，因此在 io_uring 被禁止的环境（例如 Docker 默认 seccomp 配置）无法启动。Ubuntu 6.8.0-136+ 内核会拒绝保留字全为 0 的 buffer-ring 注册（[Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)，一个写反的检查），所以注册被拒时会带 `resv[0]=1` 重试一次 |

## 设计

- **一个线程，多个 ring。** 每个 ring 有自己的固定文件表（direct descriptor），accept 直接分配进这张表；连接用 multishot receive 从 provided-buffer ring 取缓冲，空闲连接不占接收内存。ring 满之前就把接受切到下一个 ring，最多 16 个。
- **一个 pass 一个事务。** 事件循环把一次 `io_uring_enter` 之间处理的所有请求合成一个 SQLite 事务：读取共享快照，写入组提交，响应在 COMMIT 返回之后才交给内核。commit 失败时整个 pass 的响应都换成 500。
- **读走 B-tree，写走 SQL。** `GET /feed` 和 `GET /posts/:id` 直接走 SQLite 的 B-tree 游标并自行解码 record 字节；写入、`BEGIN`/`COMMIT` 和 `/health` 走普通 SQL。详见下一节。
- **checkpoint 在另一个线程。** 事件循环从不等待 fsync；WAL 帧数超过阈值时唤醒低优先级线程做 PASSIVE checkpoint，循环继续读写。
- **自带协议层。** HTTP 解析、JSON 读写与转义、base64url 解码、SHA-256/HS256 都在进程内实现，除 liburing 外没有第三方运行时依赖。
- **缓冲池。** 响应缓冲是 `app::ByteBuf`（malloc 的长度/容量结构），按请求从池里取、发送完成后归还；空闲连接只有一个固定大小的记录。
- **错误路径。** 解码遇到不认识的 record 时抛 `DbError`，每个请求捕获一次并回答 500。

## 读取走 SQLite 的 B-tree，而不是 SQL 引擎

`GET /feed` 和 `GET /posts/:id` 在 pass 的 SQLite 事务内，读 SQLite 自己的 B-tree 游标（`src/shim.c` `#include` 未修改的 amalgamation）。C++ 侧随后解码 SQLite 返回的 record 字节并直接写 JSON。没有字节码引擎，没有 `sqlite3_column_*` 调用，也没有经过 SQLite value 的拷贝。amalgamation 未修改，写入走 SQL，并保留 SQL 回退。

- **Feed：** 从 `posts_created_at_id_idx` 的第一条开始遍历。索引是 `(created_at DESC, id DESC)`，所以这正好就是 `ORDER BY created_at DESC, id DESC`，其 record 已经包含 `created_at` 和 `id`。然后按 id 升序查找这 20 篇 post。连续 id 落在同一个 leaf 的下一个 cell（SQLite 自己的 `TableMoveto` 快速路径），因此每行不需要从根到叶的下降。接下来是 `users` 里的 20 个作者。当 id 彼此相差不超过 512 时，like 计数对 `likes_post_id_idx` 做一次范围遍历，否则每篇 post 单独 seek。
- **不对数据做假设：** 该计划不需要密集 id、不需要时间戳按 id 排序，也不需要任何记忆状态。如果某篇 post 没有 user 行（SQL `JOIN` 会丢弃它），就改用参考 SQL 查询回答。启动时服务器会检查它解码的每张表和索引的列顺序，遇到不同 schema 就拒绝启动。
- **实时读取：** 每个请求在被服务时从 SQLite 读它的行（规则 5）。只复用预编译语句、游标、KeyInfo 对象和四个 B-tree 根页号（启动时从 `sqlite_schema` 读取）。`unix-excl` VFS 阻止其他进程打开文件，服务器的第二个连接只做 checkpoint，所以服务器运行期间 schema 不会变。
- **写入走 SQL：** post 用 `INSERT … RETURNING id, created_at`，like 用 `INSERT … ON CONFLICT DO NOTHING`。只有 `sqlite3_changes` 区分新 like 和重复 like。like 处理器检查 post 是否存在（404 情况）是一次在 `posts` 上的 B-tree seek，和读取一样。

## 持久化与批处理

- WAL + `synchronous=NORMAL`，通过 SQLite 的 `unix-excl` VFS 打开：进程在第一次读取时对文件加排他锁并保持，其他进程无法打开；WAL 索引驻留在进程两个连接共享的堆内存里（没有 `-shm` 文件，每个事务没有文件锁）。
- 每个事件循环 pass 是一个 SQLite 事务。它的读取共享一个快照，写入是组提交的（规则 6）。**在 COMMIT 返回之前，pass 的任何响应都不会交给内核。** 如果 commit 失败，该 pass 的每个响应都被替换成 500，并关闭连接，所以 201 绝不会声称一个没有落盘的写入。看到了本 pass 自己写入的读也会等到 COMMIT，失败则变成 500。如果 SQLite 在 pass 中途自行回滚事务（`SQLITE_FULL`、I/O 错误），该 pass 剩余部分不写，它的 COMMIT 会报告失败。
- 自动 checkpoint 关闭。checkpoint 在第二个线程、自己的连接上以 nice 19 运行，所以事件循环从不等它们的 fsync。一次 commit 留下 4,096 或更多 WAL 帧（16 MB）后，循环唤醒该线程，它运行 PASSIVE checkpoint，循环继续读写。PASSIVE checkpoint 从不等循环的锁，循环也从不等该线程。
- 在持续写入下，该 checkpoint 无法复制它运行期间写入的帧，而日志只有在完全复制后才会重启。所以当线程完成时，写入被**暂停**：每个连接上的下一个 POST 留在该连接的缓冲区里不被读取（读继续），线程复制剩下的帧。暂停随后结束，下一次写入重启日志。暂停只在 pass 之间开始和结束，绝不在 pass 内部。它最长持续 250 ms：如果线程更慢，写入继续，下一轮再试。超过 65,536 帧时，循环自己执行 checkpoint，作为最后手段。
- 不使用 `mmap`：读走 SQLite 的页缓存（64 MB）。缓存页保留已解析的 B-tree 头，查找时不需要 WAL 索引；mmap 页每次获取都要付出这两项。

## 连接

每个 k6 用户持有一个 keep-alive 连接，所以高分意味着在 `LimitNOFILE=65535` 下数十万个 socket。

- 接受的 socket 是 io_uring **直接描述符**。它们位于 ring 的文件表，而不是进程 fd 表。表最大不超过 `RLIMIT_NOFILE`，所以服务器把软限制提高到硬限制，并给每个 ring 同样多的槽位。
- 从一个 ring 开始。当某个 ring 接近满（槽位的 1/8，最多 1,024）时，接受切换到下一个 ring，必要时新增一个（最多 16 个）。在表真正满之前就切换，因为一个找不到空槽的 accept 已经把连接从 listen 队列里取走，内核会丢弃它。accept 是单发的，最多 512 个在途（小表的十六分之一），始终少于 margin，所以切换后最多只有这么多连接落地。当某个 ring 的空闲槽位超过两倍 margin 时，它重新开始接受。如果无法新增 ring，每秒重试一次。
- accept 只有在 pass 看到它完成后才重新装填，所以在途数量约束了 accept 速率。用 64 时，在 1 vCPU droplet 上每秒几千新连接的爬坡会超过它：listen 队列填满，新连接要等 SYN 重传（1 s、3 s、7 s）。512 能跟上。
- 端口有八个 `SO_REUSEPORT` listener，每个承担八分之一的 accept。一个 listen 队列上限是 `net.core.somaxconn`（默认 4,096），所以八个队列在爬坡或慢 pass 期间能容纳 32k 待处理连接，而不是拒绝它们。
- 一个线程驱动所有 ring（`SINGLE_ISSUER`、`DEFER_TASKRUN`）。每个 pass 先给其他每个 ring 一次非阻塞 `io_uring_enter`，提交它排队的发送并收集完成。然后 ring 0 提交并等待 256 个完成或 1 ms，以先到者为准。所以 pass 从不自旋，更大的 pass 共享一次 COMMIT。等待在 ring 0 上增加约 1 ms 延迟，在其他 ring 上最多约 2 ms。
- 在 `LimitNOFILE=65535` 下，每个 ring 预分配一个 65,535 项的 48 字节连接记录表（3.1 MB）和 2 MiB 接收缓冲：每个 ring 约 6 MB 用户空间，五个 ring（30 万连接）约 30 MB。表大小跟随硬描述符限制。输出缓冲池最多增加 2,048 × 32 KB。内核 socket 内存（实测每连接约 3.4 KB）在此之上。
- 每个连接有一个 **multishot receive**，从 provided-buffer ring（每 ring 1,024 × 2 KB）取缓冲，所以空闲连接不持有接收缓冲，一个请求只花一个完成。
- **突发之后，读取是有界的。** 停顿之后，每个连接都有请求在等，multishot receive 会一次用完 buffer ring，其余的以 ENOBUFS 结束。把它们全部重新装填只会一遍遍重复。所以一个结束的 receive 不会重新装填，它的连接进入先进先出的读队列。每个 pass 随后给队列里的每个连接发一个单发 receive，带 `MSG_DONTWAIT`，且每个 ring 上绝不超过它的缓冲数。读在同一次 pass 里提交和收割（在 `DEFER_TASKRUN` 下它们在非阻塞 enter 内完成），当队列比缓冲长时按轮进行。填满缓冲的读会重新入队。较短的读或 EAGAIN 表示 socket 已排空，multishot receive 重新装填。
- **先排空。** 一个 pass 最多读 16,384 次，但最多回答 2,048 次读。其余留在连接上，紧挨着半截请求，进入就绪队列，由后续 pass 优先回答，最旧的先。只要有排队，ring 0 就不等待。读的代价是与请求大小相同的拷贝，而留在 socket 里的请求代价是 3–4 KB socket 缓冲。所以 socket 队列排空的速度和读一样快，而不是堆积到 TCP 内存上限。
- 输出缓冲来自池，发送完成后归还，所以空闲连接不持有堆内存，只有一个固定的 48 字节记录（它的三个缓冲在用之前都是空指针）。每个连接同时只有一个发送在途。流水线响应排在它后面。
- 关闭是一个 `SHUTDOWN` 硬链接到一个 fixed-file `CLOSE`。shutdown 结束在途的 multishot receive 或单发读。每个 `user_data` 里的 generation 号会丢弃复用槽位的完成。
- 服务器从不关闭空闲连接（Nginx 风格的 65 s 复用是安全的）。

## 请求路径

- **HTTP：** 对 head 的行做一次遍历，找到请求行、`Authorization` 和 `Content-Length`。跨多次 receive 的请求会按连接保存，直到完整。超过 16 KB 的 head、超过 1 MB 的 body、chunked body 和格式错误的请求行会得到 400 `{"error":"bad request"}` 并关闭连接（SPEC 中超出范围）。
- **JWT：** HMAC key 的内外填充块在启动时哈希一次。每个请求只哈希 `header.payload` 加外块，并以常量时间比较结果。每个请求都检查每个 token 的签名、`alg`（仅 HS256）和 `exp`（必需）。只接受签名的规范 base64url 拼写。
- **JSON：** 一个校验读取器检查整个请求 body 并报告顶层成员，只为转义字符串分配。重复键以最后一个为准，转义的成员名会被解码，与 `JSON.parse` 一致。转义字符串只解码一次。输出字符串每次 8 字节扫描需要转义的字符，没有则整段拷贝（`-march=native` 启用时用 SSE2 16 字节和 AVX2 32 字节）。`Content-Length` 回填。
- `body` 用 JavaScript `trim()` 的空白集裁剪，并按字符（码点，与 schema 的 `CHECK (length(body) …)` 同一单位）计数。

## 文件

| | |
|---|---|
| `main.cpp` | ring、accept/receive/send/close、批量循环 |
| `src/app.cpp` | 路由、处理器、响应 |
| `src/db.cpp` | record 解码、feed 和 post 计划、写入 |
| `src/http.cpp`、`src/json.cpp`、`src/jwt.cpp` | 请求分帧、JSON、SHA-256/HS256 |
| `src/buf.hpp` | `ByteBuf` 和输出缓冲池 |
| `src/shim.c` | amalgamation 加 B-tree 游标和语句辅助、checkpoint 线程 |
| `build.sh` | 下载并校验 SQLite，然后运行 `xmake`（release、LTO） |
| `xmake.lua` | 一个 target：`src/shim.c` 用 C11，其余用 C++23，两者都 `-O3 -march=native` |

SQLite 选项（全部在 `xmake.lua` 中）：`THREADSAFE=2`（每个连接只被一个线程使用）、
`DEFAULT_MEMSTATUS=0`、`DEFAULT_WAL_SYNCHRONOUS=1`、`DQS=0`、`LIKE_DOESNT_MATCH_BLOBS`、
`MAX_EXPR_DEPTH=0`、`USE_ALLOCA`、`OMIT_DEPRECATED`、`OMIT_PROGRESS_CALLBACK`、
`OMIT_SHARED_CACHE`、`OMIT_LOAD_EXTENSION`、`OMIT_AUTOINIT`、`OMIT_UTF16`、`OMIT_TRACE`、
`OMIT_COMPLETE`、`OMIT_GET_TABLE`、`OMIT_TCL_VARIABLE`、`HAVE_FDATASYNC`、`HAVE_USLEEP`。

## 本地验证与数字

在 Ubuntu 24.04 容器（g++ 13.3、liburing 2.5、内核 6.18、8 vCPU）中针对 seed 数据库运行。k6 和服务器共用这台机器，所以这些不是基准机的数字。

- `test/test.sh`：**42/42**。
- 跨两次写入的请求、一次写入的两个流水线请求、body 分两段到达的 POST：全部正确回答。
- `bench/load.js` 在 **8,000 VUs**（30 s 爬坡，45 s 保持），描述符表每 ring 上限 4,096（`ulimit -n 4096`），所以接受在保持期间必须新增两个 ring（`ring 1 added`、`ring 2 added`）：**0.00% 失败请求**，p95 2.87 ms，p99 3.61 ms，峰值 RSS 26 MB。
- 1,024 个 keep-alive 连接对 `GET /feed` 做无思考时间循环，服务器绑在一个 2.4 GHz 核上：**43k req/s**，每请求 21 µs CPU。`perf` 显示约一半 CPU 在内核 TCP/io_uring 路径；用户态最大项是 JSON 转义（9%）、feed 的 B-tree 行解码（7%）和整数格式化（6%，所以现在数字按实际长度拷贝，而不是固定 20 字节）。

## 运行

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
