# C++23，无框架：io_uring + SQLite B-tree 读取

一个单线程的 HTTP 服务：五个接口，JSON 进 JSON 出。io_uring 事件循环、HTTP/1.1 解析、JSON 读写、HS256 校验和 SQLite B-tree 读取都在同一个线程里完成；SQLite 使用固定版本、未修改的 amalgamation，静态链接。

| | |
|---|---|
| 语言 | C++23，g++ 13（Ubuntu 24.04 `build-essential`） |
| 构建 | xmake（由 `install.sh` 安装），release 模式 + LTO；有 mold 时用 mold 链接 |
| 依赖 | liburing 2.5（Ubuntu 24.04 `liburing-dev`）、SQLite 3.54.1 amalgamation（sha256 固定，`build.sh` 下载，本地编译） |
| Nginx 还是直连 | **直连**：自己监听 `HOST:PORT`（基准机上为 `0.0.0.0:80`） |
| 要求 | Linux 6.1+（io_uring 的 `DEFER_TASKRUN`、multishot receive、provided-buffer ring）。没有 epoll 回退，因此在 io_uring 被禁止的环境（例如 Docker 默认 seccomp 配置）无法启动。Ubuntu 6.8.0-136+ 内核会拒绝保留字全为 0 的 buffer-ring 注册（[Launchpad #2162843](https://bugs.launchpad.net/ubuntu/+source/linux/+bug/2162843)，一个写反的检查），所以注册被拒时会带 `resv[0]=1` 重试一次 |

## 架构

按职责分层，热路径上全部是编译期可内联的类型，没有虚函数：

| 层 | 文件 | 职责 |
|---|---|---|
| io | `src/io/ring.hpp` | `Ring`：io_uring 的 RAII 封装（setup、稀疏文件表注册、SQE、提交、等待）；`BufRing`：provided-buffer ring 的注册、发放与发布 |
| net | `src/net/connection.hpp` | `Connection`：一个槽位的读写状态机；`SlotTable`：固定槽表 + 代际 |
| runtime | `main.cpp` 的 `Worker` / `Server` | 每个 ring 一个 `Worker`；`Server` 负责 poll、drain、批处理、事务与发送 |
| db | `src/db.hpp`、`src/shim.c` | `Db` 是事务和读取计划的入口；`dbx_*` 是 SQLite 的 C 访问层 |
| http | `src/http.hpp` | 无状态的请求分帧 |
| json | `src/json.hpp` | 校验式读取、转义、整数格式化、输出游标 |
| jwt | `src/jwt.hpp` | SHA-256/HS256、base64url、claims 校验 |
| app | `src/app.hpp` | 路由与五个处理器 |

**连接状态机。** 每个连接有两个正交的状态：`ReadState`（`Idle` 表示 multishot receive 已装好，`Queued` 表示等单发读，`Reading` 表示单发读在途）和 `WriteState`（`Idle`/`Sending`），加上 `open`/`closing`（关闭已提交）/`close_after`（待关闭）三个布尔。状态转移集中在 `drain` 的 switch 里，不再用位标志表达。

**批处理。** 一个 pass 是：对每个 ring 提交并收集完成（ring 0 在无积压时等 256 个完成或 1 ms，有积压时非阻塞；其余 ring 只提交、不进入内核）→ 每个 ring 最多处理 **2,048** 个完成（读远多于答：读一个请求的成本是它那么大的一次拷贝，留在 socket 里要占 3–4 KB 内核缓冲，所以 socket 要尽早抽干，超出的回答进就绪队列；rust-iouring-kensac #264 与 c-iouring-jowinjestine #404 的读配额都是 16,384 配 2,048 的答案） → **同一 pass 内把排队的单发读「发出去 → 收回来 → 答掉」**（最多 4 轮，且只发本 pass 还答得掉的个数） → 组提交（见下「写窗口」）→ 把这一批的响应交给 io_uring 发送。看见自己那个窗口里写入的读等 COMMIT；commit 失败时整批响应替换成 500。**一个 pass 最多回答 256 个请求**（fsharp-uring-delneg #437 的 ANSWERS）：2,048 是错的——一个请求 27 µs，2,048 个就是 55 ms，pass 本身就长，排队一叠加 p99 就失控（280k 探针 5.6 s）；**256** 是折中：一个 pass 约 6.9 ms，COMMIT 从 64 时的 391 次/秒降到 98 次/秒，本地 20k 连接的 p99 从 227 ms 降到 87.6 ms，超出的连接连同已读字节留在自己的 `part` 缓冲里、进就绪队列；读本身不受此限，socket 还是要尽快抽干。`ANSWERS=` 可以覆盖这个上限（310,000 那次保持用的是 2,048，见下文「本地验证」）。

**同一 pass 收读（read rounds）。** 单发读不再在 pass 末尾发出去等下一个 pass 才收：每轮「给排队的连接各发一个 `MSG_DONTWAIT` 读 → 非阻塞 `io_uring_enter` → `drain`」，最多 4 轮。只发本 pass 还答得掉的个数（`reads_budget_ = answers_left_`），所以多读进来的数据一定答得掉，不会被 `defer` 成一块 `part` 缓冲。原来那版每个走单发读的连接都要多等一个完整 pass（低负载 1 ms 的等待，突发时是整个 backlog 的长度）；c-iouring-pavlyhalim #245 量过两者的差别：*在批末尾发出去的读要多等一批加最多 2 ms：p95 4.7 对 2.7 ms（20,000 用户）*，kensac #264 的 `read_rounds` 是同一形状，他们把它记成 270k → 310k 的那一步。

**backlog 的顺序。** 就绪队列是 **LIFO**：刚被延后的连接下一个 pass 先答，所以大量请求保持快。等过 **8 秒**的请求**拿一半的额度**（从队尾取最老的），**等过 16 秒的全部优先**（objc-uring-delneg #326 的第二级），没人被饿死；**不能整批取队尾**——backlog 持续时队尾会一直处于老化状态，全取队尾就退化成最老优先，新请求又被拖慢（objc-uring-delneg #326 的原话是 *the oldest get half of each pass*，基准机 230k 探针 p99 3.08 s 正是整批 FIFO 的形状）。这条来自 #326 的论证——回复晚于请求 40 ms 要多一个 bare ACK、约每请求 +20% CPU，而最老优先会把这个代价摊给每个新请求，多出的 CPU 又帮着把 backlog 撑住；230k ramp 期所有新连接同时发第一个请求，那正是「p99 数秒、0 错误」的形状。

**ring 模型。** ring **按需增长**：起步 1 个，最新一个的表**半满**时加下一个（一次补齐多个，ramp 不落后），上限 **16**。ring 数量按目标连接数推导：`ceil(400,000 / 每 ring 槽位)`。每 ring 的槽位上限是 262,144，但**实际取 `min(硬 RLIMIT_NOFILE, 262,144)`**——基准机 `LimitNOFILE=65535` 时就是 7 个 ring × 65,535 = **458,745 连接**，`NOFILE=1M` 时 2 个 ring × 262,144 = 524,288。表尺寸不按 `RLIMIT_NOFILE` 一次预分配：旧版按 RLIMIT 预分配 6 × 1 M 项曾让常驻内存多出 288 MB，在 2 GB 的机器上就是 300k 连接装不装得下的区别。每 pass 的 `io_uring_enter` 是 1 次/ring；非 0 号 ring 不设 `DEFER_TASKRUN`、只提交不取事件，所以环数增加几乎不加系统调用（3 ring 存活时实测 `enter[wait=1040 sqe=0 ring=66]`）。

**listen。** 端口上固定 **8 个共享的 `SO_REUSEPORT` listener**（与 ring 数无关），每个有自己的 accept 队列（`somaxconn` 4,096），ramp 期合计约 32k 个待建连接；每个 ring 对每个 listener 保持 **64** 个单发 accept 在途（每 ring 固定 512，**不按 ring 数稀释**——ring 数正是随连接数增长的，300k 时约 10 个，按总数除下去每 listener 会从 51 掉到 32，再多就撞上 `per<16` 的下限而停投）`full()` 的阈值也随之放宽到 `open + 512`，ring 顶端的 1,536 个槽位不再被提前判掉（310k/320k 正卡在这个边界）。ring 满时只停止投递 accept，不关闭共享 listener；**等 open 回落到「两个 margin」以下再恢复**（`open + 2*kAcceptDepth < slots`，kensac #264 的恢复条件）——早先只有停、没有恢复，一次短暂的表满就会永久拒绝新连接（270k/280k 的 connection refused）。

**数据层。** `dbx_*` 直接走 SQLite 的 B-tree 游标：feed 从 `posts_created_at_id_idx` 顺序取 20 个 id，再在 `posts`/`users` 里按 id 查找（20 个 id 跨度 < 512 时用一次 seek 加顺序扫描，否则退回逐个 seek；阈值与 rust-iouring-kensac #264 的 512 对齐）；like 计数在**进程内**维护：第一次 `begin()` 时借游标做一次全表直方图（`dbx_likes_range` 只解首尾两个 cell 取最小/最大 post_id，`dbx_likes_fill` 顺序走一遍 `likes_post_id_idx` 把每组的行数记进 `std::vector<unsigned>`；seed 库 383,858 组 / 2,016,005 行，500,000 项约 2 MiB 常驻），之后 `INSERT` 新 like 时同步自增，`GET /feed` 的 20 个计数是 20 次数组读。原来的做法是沿 `likes_post_id_idx` **在游标持有的页上直接解 cell**（`findCell` + 一字节长度/小整数头部检查），20 个 id 之间的每个 cell 都要解一遍，gprof 里占 **8.75 %**——那段扫描（`dbx_likes_many`）保留为直方图失败时的退路；20 个作者用 20 次 `sqlite3BtreeTableMoveto`（按 rowid 升序，局部性最好）。这条页级扫描与 rust-iouring-kensac #264 的 `td_count_likes` 同形：页由游标持有，不获取也不释放页面；格式超出假设的 cell 返回 1，调用方退回「每个 post 一次 seek」。checkpoint 在独立的低优先级线程上、用自己的连接做 PASSIVE checkpoint，事件循环不等 fsync。SQLite 以 `THREADSAFE=2` 编译，页缓存 16 MiB，`mmap_size=0`，并套一层 `SQLITE_CONFIG_PCACHE2` shim 跳过 pcache1 的整表扫描。

**JWT 的 SHA-256 压缩调用 libcrypto。** `SHA256_Transform` 是 libcrypto 导出的符号，且只读写 `ctx->h`——而 `h` 是 `SHA256_CTX` 的第一个成员，所以把自己的 8 字状态直接传进去即可，增量哈希和 HMAC 的结构一行都不用改，OpenSSL 的 CPU 派发会自己挑 AVX2/AVX-512/SHA-NI 路径（fsharp-uring-delneg #437 同样是走 OpenSSL 的 SHA-256）。用 `dlopen("libcrypto.so.3")` 在第一次 `Sha256::create()` 时解析，所以**编译期不需要 libssl-dev、二进制也不链接 libcrypto**；解析不到就退回本文件的标量实现，行为一致。本地实测 JWT 验证 2.03 → 1.55 µs。

**C++23 编译期计算。** 响应头由 `consteval` 在编译期拼成 `std::array`（状态行 + Content-Type + Content-Length 占位），运行时一次拷贝；JSON 转义表、base64url 表、两位数字表和 serial-type 长度表都是 `constexpr`；record 里的整数用 `std::byteswap` + 位运算解码，输入用 `std::string_view`/`std::span` 表达。

**PGO。** `build.sh` 默认跑两遍：先用 `-fprofile-generate` 编一版，起一个临时实例（端口 3987、临时库）跑完整的 42 项检查加 300 次 `/feed` 训练，再用 `-fprofile-use` 重编——分支布局和内联决策据此优化，和 c-iouring-public 的 `PGO=0` 开关是同一套做法。训练只在 shell + curl 上完成（CI 没有 k6），任何一步失败就退回普通 `-O3` 构建；`PGO=0 ./build.sh` 可跳过。

**微架构与构建。** 编译用 `-O3 -march=native -fno-plt -fno-semantic-interposition -flto`，另加 `-falign-functions=32 -falign-loops=32`、`-fipa-pta`、`-flto-partition=one`；io_uring 的 ring 启用 `COOP_TASKRUN`/`TASKRUN_FLAG`，连接表和接收缓冲用 `MADV_HUGEPAGE` + `MADV_POPULATE_READ` 预触；有 mold 时用 `-fuse-ld=mold` 并行链接。

**来源说明。** io_uring 的 multishot receive 与 ring 模型参考了 rust-iouring-kensac (#264) 的实现思路；**direct-descriptor 表大小跟随硬 `RLIMIT_NOFILE`（上限 262,144）也是 #264 的做法**——固定大表在 `NOFILE` 较小的 CI runner 上会被 `io_uring_register_files_sparse` 以 `EMFILE` 拒绝，这一点是照着他们改的。**8 个共享 `SO_REUSEPORT` listener 同样来自 #264**（`LISTENERS = 8`，与 ring 数无关）。**backlog 的 LIFO + 8 秒老化来自 objc-uring-delneg (#326)** 的论证（他们记的是每请求 +20 % CPU 的 bare ACK 代价），不是 #264 的最老优先。**drain 中途 publish 接收缓冲、以及「发读 → 收读」在同一 pass 内按轮完成**改写自 c-iouring-jowinjestine (#486/#404) 的中途 `ring_buf_publish`/`reap_all` 与 rust-iouring-kensac (#264) 的 `read_rounds`；后者的 270k → 310k 就是这一步。**两段式检查点**（先不挂写拷大块、只剩尾段才挂写、尾段用普通优先级线程、超过 65,536 帧由循环兜底）同时见于 #264 与 #404/#486，hold 上限 400 ms 取自 #486；**闸门阈值 32,768 帧与本实现的 20 ms 写窗口是在基准机诊断出写尾（likes p99 4.9 s）之后自己调的**，20 ms 这个量级取自 fsharp-uring-delneg #437 的 `WWIN`。`MSG_WAITALL` 发送取自 #486/#245。数据层只用 B-tree 游标 API（`sqlite3BtreeFirst/Next/TableMoveto/IndexMoveto/Payload*`）；like 计数的页级 cell 扫描与 #264 的 `td_count_likes` 同形（读游标持有的页，不自己获取/释放页面），此前那版批量作者 seek（每层共享页、页内插值二分）改写自 c-iouring-pavlyhalim (#245) 的 `src/btree.h`，因为走的是 SQLite 内部页函数、落在「B-tree cursor reads are fine」这条裁定之外，已经删掉换成 20 次 `TableMoveto`（实测 handler 8.86 → 8.34 µs，反而更快）。WAL 写合并 VFS（`src/shim.c` 的 `wf_*`/`wv_*`）与 page-cache 的 `SQLITE_CONFIG_PCACHE2` shim（`wc_*`）同样改写自 #245 的 `src/db.h`（同一段代码，改了命名），`WAL_COALESCE=0` 可关掉 VFS 做 A/B（仅供测试，规则里没这条）。record 的变长整数与列头解码沿用 #245 的写法。like 计数在进程内维护（启动时一次全表直方图、写入时自增）也是本实现自己的做法，榜单上没有对应来源。除此之外的事件循环、状态机、批处理、事务与 checkpoint 策略、HTTP/JSON/SHA-256 协议层是本实现独立编写的。

## 读取走 SQLite 的 B-tree，而不是 SQL 引擎

`GET /feed` 和 `GET /posts/:id` 在 pass 的 SQLite 事务内，读 B-tree 游标（`src/shim.c` `#include` 未修改的 amalgamation）。C++ 侧解码 record 字节并直接写 JSON：没有字节码引擎，没有 `sqlite3_column_*` 调用，也没有经过 SQLite value 的拷贝。

- **Feed：** 从 `posts_created_at_id_idx` 的第一条开始遍历。索引是 `(created_at DESC, id DESC)`，正好是 `ORDER BY created_at DESC, id DESC`，record 里已经带着 `created_at` 和 `id`。然后按 id 升序在 `posts` 里查找这 20 篇，连续 id 落在同一个 leaf 的下一个 cell（`TableMoveto` 的快速路径）。接下来是 `users` 里的作者。like 计数直接读进程内的直方图（见「数据层」），不再为每个请求重扫索引。
- **实时读取：** 每个请求在被服务时从 SQLite 读它的行（规则 5）。只复用预编译语句、游标、KeyInfo 和四个根页号（启动时从 `sqlite_schema` 读取）。`unix-excl` VFS 阻止其他进程打开文件。
- **写入走 SQL：** post 用 `INSERT … RETURNING id, created_at`，like 用 `INSERT … ON CONFLICT DO NOTHING`；只有 `sqlite3_changes` 区分新 like 和重复 like。like 处理器检查 post 是否存在是一次 `posts` 上的 B-tree seek。

## 持久化与批处理

- WAL + `synchronous=NORMAL`，通过 `unix-excl` VFS 打开；WAL 索引在进程内共享，没有 `-shm` 文件。WAL 文件上再套一层写合并 shim：帧头和页连续写时先攒进缓冲，commit 帧的页到达时一次 `pwrite` 落地（写序不变，commit 帧在 COMMIT 返回前已在内核里）。本地 A/B 里它没量出差别（本实现已按 pass 组提交，每请求 WAL 写只有 ~0.2 次，合并的理论收益约 0.6%），保留是因为它已被 #245 在基准机上量到 0.24 → 0.095 pwrite/请求。
- 每个事件循环 pass 是一个 SQLite 事务：读共享快照。**COMMIT 返回之前，看见写入的响应不会交给内核。** commit 失败时这些响应都变成 500。
- **写窗口（组提交）。** COMMIT 不是每 pass 一次，而是**最多每 20 ms 一次**（`kCommitWindowNs`，`COMMIT_WINDOW_MS=` 可覆盖，0 = 关掉退回每 pass 提交）。窗口没到点时，本 pass 的写连接连同还没发的响应一起挂进 `pend_commit_`（带 `gen` 防槽位复用），读连接照常答；窗口到点才 COMMIT 一次，把攒下的写响应一起放出。理由：285k 时约 2,100 写/秒、每个写只沾 0.2 个请求，按 pass 提交会让**绝大多数提交是空的**，而每个非空提交要动 4–7 页——本地 `NO_CKPT=1` 直接量到 WAL 帧率 **2,141/秒**（8.4 MB/s），闸门因此每 1–2 秒关一次，每次关都挂写。本地 A/B（800 VUs、`bench/mixedonly.js` 无 think time、同一二进制只改 `COMMIT_WINDOW_MS`）：COMMIT 348 → **42 次/秒（−88%）**、WAL 帧 2,141 → **1,415/秒（−34%）**、吞吐 13,799 → **14,351 req/s（+4%）**。响应顺序不受影响：响应是追加进 `c.out` 的，推迟只会让同一连接后来的响应排在它后面。
- Auto-checkpoint 关掉。检查点是**两段式**的（`src/shim.c` 的 `CK_*` 状态机，结构照 c-iouring-jowinjestine #404/#486 与 rust-iouring-kensac #264）：
  1. **BULK**：WAL 到 **32,768 帧（128 MiB）**时**不挂写**，nice 19 的线程连着做 `PASSIVE`，每次拷的是「上一次 pass 之后又写进来的帧」，直到落后不超过 **64 帧**；
  2. **TAIL**：这时才挂写（写请求留在连接上，读照常答），由**普通优先级**的线程把剩下的几十帧拷完，几毫秒结束；全部拷完之后下一次写让日志从第 1 帧重新开始，文件就停在那里——不需要 `TRUNCATE`，也不需要 `RESTART`。
  旧做法是「WAL 一过 4,096 帧就挂写，再由同一个 nice 19 线程把整段 16 MB 以上的积压拷完」，一次 hold 就是几百毫秒——而基准机上 300k/320k 的探针正是「0 错误、p99 4.45 s」的形状：写请求被一次次数百毫秒地挂起，k6 的 VU 是串行的，同连接的后续请求跟着一起等。现在**挂写的时间从「整个 16 MB 的拷贝」缩到「尾段」**，而尾段线程和主循环同优先级，不会被 nice 19 拖长。local hold 上限 **400 ms**（日志超过 32,768 帧时放宽到 800 ms）；超过 **65,536 帧**属于线程追不上的情况，事件循环自己拷一次兜底。**闸门阈值从 4,096 帧抬到 32,768 帧（`CK_GATE=` 可覆盖）**：4,096 在评测机约 4,000 帧/秒下等于每 1–2 秒进一次 TAIL，也就是每秒挂一次写——基准机的写尾（likes p99 4.9 s、new posts 4.5 s）与这个频率吻合；32,768 把它摊到 8–12 秒一次，代价是 WAL 峰值多 112 MiB。这一条**本地测不出差别**（本地 `gated` 恒为 0，从没挂过写：30 s 跑 32,768 好 6.4 %、60 s 跑反而差 1.1 %，方向都不一致），是按机制推的，不是本地实测结论。**拷之前给整段日志做 `readahead`、以及拷完之后 `fadvise(DONTNEED)` 丢掉 WAL 的页缓存，这两个都试过并在本地成对 A/B 里被否掉**：日志帧本来就在页缓存里，readahead 白走一遍 16 MB 范围把吞吐从 9.9k 压到 8.9k；DONTNEED 让下一次检查点重新从磁盘读，尾段反而变长（16 MB 的页缓存对 2 GB 的机器也不值得省）。本地 8,000 VUs 混合负载（7 % 写）：COMMIT 占总循环时间从 38–58 % 降到 **4–8 %**，单次 COMMIT 最大值从 20–74 ms 降到 **1–11 ms**，`gated` 从 276/472 掉到 **0/946**（写再也不需要挂起）。
- 事件循环的等待是有条件的：只要还有待发的读、待写的响应或上一 pass 收到的完成，就 `min_complete=0` 直接 `enter`，**不睡那 1 ms**；只有在完全没事干时才等（1 ms）。这一条是 220k 时 29% CPU 空转的来源。
- 空闲连接清扫按时间切片（每 50 ms 扫一个 ring 的 1/32），不在 pass 里整表扫 6 M 个槽位——一次整扫在 1 vCPU 上是 25–70 ms，正好是 p99 悬崖。
- **page cache 外面套一层 `SQLITE_CONFIG_PCACHE2` shim**（来自 #245）：`pcache1` 记住"曾经持有过的最大页号"且从不调低，而 B-tree split 会把一页临时改名到 lock-byte 页号再改回来，于是此后**每次 COMMIT** 的 `xTruncate` 都以为文件尾之外可能有页、把整个 hash 表扫一遍（#245 量到 ~350 µs、~30 次/秒）。shim 记录进入缓存的最大 key（停在 lock-byte 页号的页单独计数），确定不可能有越界页时直接跳过扫描。本地饱和 `/feed`：50.0 → **44.5 µs/请求（−11 %）**。
- **页缓存 16 MB**（`cache_size=-65536`）：2 GB 机器上要给内核的 socket 内存留位置；16 MB 仍然装得下 feed 的工作集（users 表 + 最新的 posts 页）。64 MB 时的本地 5 分钟保持 p99 是 2.1 ms、16 MB 是 4.2 ms，都远在 1 s 阈值内，换来 RSS 少 14 MB。
- No `mmap`: reads go through SQLite's page cache (16 MB).

## 连接

每个 k6 用户持有一个 keep-alive 连接，所以高分意味着在 `LimitNOFILE=65535` 下数十万个 socket。

- 接受的 socket 是 io_uring **直接描述符**：它们位于 ring 的文件表，不占进程 fd 表。表大小跟随硬 `RLIMIT_NOFILE`（上限 262,144），所以同一个二进制在 `NOFILE=65535` 的 CI runner 上会注册 65,535 项并正常起来，在基准机上拿满 262,144。
- **listener 上关掉 `TCP_QUICKACK`**（接受连接继承）：连接建立后的第一个请求由响应顺带确认，而不是内核先补一个单独的 bare ACK，ramp 期每个连接少一个包（fsharp-uring-delneg #437）。
- **listener 上设 `SO_RCVBUF=16384`**，accept 出来的连接继承（`ss -tin` 实测服务器侧 `rcv_space:16384`，客户端侧仍是默认值）。默认的自动调优会一路涨到上百 KB，2 GB 的机器在 300k 连接时正是被这一项顶到内存墙上（c-iouring-jowinjestine #486 对同一件事的处理：平时 16 KiB；**PSI 高压时降到 8 KiB**——每 2 秒读一次 `/sys/fs/cgroup/memory.pressure` （full avg10 ≥ 2 % 或 some avg10 ≥ 10 %）与 cgroup 用量（>85 % 上限），连续两次高压才降、连续四次低压才恢复，且只影响**未来 accept** 的连接，绝不节流或关闭已有连接（c-iouring-jowinjestine #486 的迟滞规则）。请求只有一两百字节，16 KiB 放得下几十个；`RCVBUF=` 可覆盖。
- 端口上是 **8 个共享的 `SO_REUSEPORT` listener**（固定 8 个，与 ring 数无关），每个有独立的 accept 队列；ring 按需增长（1 → 最多 16），每个 ring 对每个 listener 保持 `depth/8` 个单发 accept 在途。ring 满时停止投递 accept，其他 ring 继续从全部 8 个 listener 接收。
- Each connection has one **multishot receive**; buffers come from a provided-buffer ring (2048 × 2 KB per ring), so an idle connection holds no receive memory.
- **接收缓冲在 drain 中途就交还内核**：每处理 256 个完成就把 CQ 头推进一次、把 buffer ring 的尾指针发布一次（原来整批 2,048 个处理完才发布一次，而 buffer ring 正好 2,048 项——一个 drain 就能把接收缓冲用光，剩下的 multishot receive 全部以 `ENOBUFS` 结束、被塞进 pending 队列等下一个 pass 的单发读）。c-iouring-jowinjestine #486 的中途 publish 是同一个动机，他们量到 2.15M → 119k 次 ENOBUFS、最长的一次循环轮次 1.9 s → 0.65 s。
- 突发之后 multishot receive 仍然可能耗尽 buffer 并以 ENOBUFS 结束；这样的连接进入 ring 内的 FIFO 队列，每个 pass 给它们发单发读（`MSG_DONTWAIT`，且每个 ring 不超过 buffer 数），并在同一个 pass 内收回来（见上）。填满 buffer 的读重新入队，短读或 EAGAIN 则重新装好 multishot receive。
- **大响应用零拷贝发送**（IORING_OP_SEND_ZC）：feed 的 JSON 约 5.5 KB，send_zc 让内核直接读用户缓冲，省掉一次 5.5 KB 的拷贝；完成时会多一个 IORING_CQE_F_NOTIF，缓冲必须等它到达才能还回池（连接状态多了 Flushed）。post/like 的响应只有几百字节，多一个 NOTIF 不划算，仍走普通 send，并带 **`MSG_WAITALL`**：io_uring 自己把短写补完，完成就是全部发完，省掉一次「再挂一个 send」的 SQE 和完成（c-iouring-jowinjestine #486 与 c-iouring-pavlyhalim #245 的发送都是 `MSG_NOSIGNAL|MSG_WAITALL`）；关闭中的响应也不用。内核不支持时（-EINVAL/-ENOSYS）自动回退并永久关闭，ZC=0 可手动关掉做 A/B。还有两条边界：第一个完成若没有 IORING_CQE_F_MORE，说明不会再有 NOTIF，必须立刻视作已通知；以及**普通 send 根本不会有 NOTIF**，它的完成必须直接收尾——错判成 Flushed 会让连接永远等下去（响应其实已经发出，所以 42 项测试照过，但 keep-alive 的下一个请求就超时）。
- 每个连接同一时间最多一个发送在途；流水线响应排在它后面。输出缓冲来自池（8 KB，启动预填 256 个），发送完成后归还；一次 feed 响应的 JSON 上界约 24 KB，所以回收阈值放到 `8 × 8 KB`，否则每个 feed 缓冲都会被丢掉重分配（实测：每请求一次 24 KB `malloc`，`brk` 37,738 次/10 s）。
- 关闭是 `SHUTDOWN` 硬链接到 fixed-file `CLOSE`；每个 `user_data` 里的 generation 号会丢弃复用槽位的完成。
- **`part` 缓冲保持 `std::vector`（不用 pmr 池）**：试过 `std::pmr::vector` + `unsynchronized_pool_resource`，碎片确实少了，但池会持有空闲块不归还（`release()` 又不能用，ready 队列空不代表没有存活的 `part`），270k 连接时实测内存 1,214 MB，比 `std::vector` + `malloc_trim` 的 1,097 MB 还高 117 MB，因此回退。
- **一个 malloc arena**（`mallopt(M_ARENA_MAX, 1)`）：事件循环和 checkpoint 线程轮流跑，各自一个 arena 就是两份高水位，而 glibc 在 free 之后不会把 arena 缩回去（c-iouring-raoz 的做法）。
- **backlog 清空后把空闲堆还给内核**：backlog 清空时每 10 秒调用一次 `malloc_trim(0)`；**backlog 一直不断时也每 30 秒调用一次**（原来只要还有积压就永远不 trim，而 270k 的保持期恰恰一直是那种状态，堆只涨不还）。glibc 释放的内存默认留在进程 arena 里，RSS 只增不减——backlog 期间为 `part`/响应缓冲分配的块，在 backlog 清空后仍算在 RSS 上（c-iouring-jowinjestine #404 把这一条记成「backlog 碰过的每个连接都永久留着一块」，他们观测到 200k 个 deferred 请求约 100 MB）。本地 20k 连接、标准负载：p99 196 → 2.78 ms，max 655 → 39 ms。

## 请求路径

- **HTTP：** 对 head 的行做一次遍历，找到请求行、`Authorization` 和 `Content-Length`。跨多次 receive 的请求按连接保存，直到完整；超过 16 KB 的 head、超过 1 MB 的 body、chunked body 和格式错误的请求行返回 400 并关闭连接。
- **JWT：** HMAC key 的内外填充块在启动时哈希一次；每个请求只哈希 `header.payload` 加外块，常量时间比较。每个请求都检查签名、`alg`（仅 HS256）和 `exp`（必需）。
- **JSON：** 校验式读取器检查整个请求 body，只为转义字符串分配；重复键以最后一个为准。输出字符串用 SIMD 扫描（AVX2 32 字节 / SSE2 16 字节），无转义则整段拷贝；整数按实际长度拷贝。
- `body` 用 JavaScript `trim()` 的空白集裁剪，并按码点计数。

## 文件

| | |
|---|---|
| `main.cpp` | `Worker`/`Server`：poll、drain、批处理、事务与发送 |
| `src/io/ring.hpp` | `Ring`/`BufRing` 的 RAII 封装 |
| `src/net/connection.hpp` | `Connection` 读写状态机、`SlotTable` |
| `src/app.cpp` | 路由、处理器、响应 |
| `src/db.cpp` | record 解码、feed 和 post 计划、写入 |
| `src/http.cpp`、`src/json.cpp`、`src/jwt.cpp` | 请求分帧、JSON、SHA-256/HS256 |
| `src/buf.hpp` | `ByteBuf` 和输出缓冲池 |
| `src/shim.c` | amalgamation 加 B-tree 游标访问层 |
| `build.sh` | 下载并校验 SQLite，然后运行 `xmake`（release、LTO） |
| `xmake.lua` | 一个 target：`src/shim.c` 用 C11，其余用 C++23，都 `-O3 -march=native` |

SQLite 选项（全部在 `xmake.lua` 中）：`THREADSAFE=2`、`DEFAULT_MEMSTATUS=0`、
`DEFAULT_WAL_SYNCHRONOUS=1`、`DQS=0`、`LIKE_DOESNT_MATCH_BLOBS`、`MAX_EXPR_DEPTH=0`、
`USE_ALLOCA`、`OMIT_DEPRECATED`、`OMIT_PROGRESS_CALLBACK`、`OMIT_SHARED_CACHE`、
`OMIT_LOAD_EXTENSION`、`OMIT_AUTOINIT`、`OMIT_UTF16`、`OMIT_TRACE`、`OMIT_COMPLETE`、
`OMIT_GET_TABLE`、`OMIT_TCL_VARIABLE`、`HAVE_FDATASYNC`、`HAVE_USLEEP`。

## 本地验证与数字

在 Ubuntu 24.04 容器（g++ 13.3、liburing 2.5、内核 6.18、8 vCPU）中针对 seed 数据库运行。k6 和服务器共用这台机器，所以这些不是基准机的数字。

- `test/test.sh`：**42/42**。
- 跨两次写入的请求、一次写入的两个流水线请求、body 分两段到达的 POST：全部正确回答。
- `bench/load.js` 在 **8,000 VUs**（30 s 爬坡，45 s 保持）：**0.00 % 失败请求**，p95 3.17 ms，p99 3.92 ms。
- 10 万条 keep-alive 长连接、每 3 秒一次 `GET /feed`（约 30k req/s）：**0 错误、0 断连**。
- 单核无思考时间循环 `GET /feed`（1,024 连接）：约 35k req/s；`perf` 显示约一半 CPU 在内核 TCP/io_uring 路径。
- **混合负载 A/B**（`bench/load.js` 的请求比例、去掉 think time，8,000 VUs × 40 s，服务器钉在 0 号核、k6 用 1–7 号核；两次交替测，数字是同一次机器状态下的对照）：改动前 6.8k req/s、p95 1.78 s、COMMIT 占循环 38–58 %、单次 COMMIT 最大 20–74 ms；改动后 **9.0k req/s、p95 1.47 s、COMMIT 占 4–8 %、最大 1–11 ms**，`enobufs=0`（接收缓冲不再耗尽）、`gated=0`（写从未被挂起）。这台容器的磁盘和宿主机是共享的，绝对值不可与基准机比较，只能同机对照。
- `STATS=1` 会每秒打印一次分项：`pass/s`、`enobufs`、`gated`、wait/drain/read/commit 各自占比与 **COMMIT 最大值/超 5 ms 次数**。

## 运行

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

A/B 用的环境变量（都有默认值，不设就是上面的行为）：`ANSWERS=` 每 pass 答案上限（默认 256；
310,000 那次保持用的是 2,048）、`ZC=0` 关零拷贝发送、`RCVBUF=` 接收缓冲上限、
`TCP_MEM_GOVERNOR=0` 关掉 PSI 治理、`WAL_COALESCE=0` 关掉 WAL 写合并 VFS、`STATS=1` 打分项统计。
