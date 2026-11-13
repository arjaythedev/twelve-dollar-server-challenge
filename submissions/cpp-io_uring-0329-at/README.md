# C++23，无框架：io_uring + SQLite B-tree 读取

一个单线程的 HTTP 服务：五个接口，JSON 进 JSON 出。io_uring 事件循环、HTTP/1.1 解析、JSON 读写、HS256 校验和 SQLite B-tree 读取都在同一个线程里完成；SQLite 使用固定版本、未修改的 amalgamation，静态链接。

| | |
|---|---|
| 语言 | C++23，g++ 13（Ubuntu 24.04 `build-essential`） |
| 构建 | xmake（由 `install.sh` 安装），release 模式 + LTO |
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

**批处理。** 一个 pass 是：对每个 ring 提交并收集完成（ring 0 等待 256 个完成或 500 µs，其余非阻塞）→ 每个 ring 最多处理 256 个完成 → 给排队的连接发单发读 → `Db::commit()` → 把这一批的响应交给 io_uring 发送。看见本 pass 写入的读也等 COMMIT；commit 失败时整批响应替换成 500。

**ring 模型。** 固定 8 个 ring，每个 ring 有自己的一张 65,535 项文件表和**一个专属的** `SO_REUSEPORT` listener。ring 接近满时关闭自己的 listener（连接由其他 ring 接收），不做运行期扩容或跨 ring 迁移。

**数据层。** `dbx_*` 直接走 SQLite 的 B-tree 游标：feed 从 `posts_created_at_id_idx` 顺序取 20 个 id，再在 `posts`/`users` 里按 id 查找；like 计数把 20 个 id 排序后沿 `likes_post_id_idx` 走一遍（merge），一次遍历统计全部 20 个。checkpoint 在独立的低优先级线程上、用自己的连接做 PASSIVE checkpoint，事件循环不等 fsync。SQLite 以 `THREADSAFE=2` 编译。

**C++23 编译期计算。** 响应头由 `consteval` 在编译期拼成 `std::array`（状态行 + Content-Type + Content-Length 占位），运行时一次拷贝；JSON 转义表、base64url 表、两位数字表和 serial-type 长度表都是 `constexpr`；record 里的整数用 `std::byteswap` + 位运算解码，输入用 `std::string_view`/`std::span` 表达。

**来源说明。** io_uring 的 multishot receive、direct descriptor 多 ring 以及 SQLite B-tree 游标的用法参考了公开的 io_uring/SQLite 实践，包括 #264 的实现思路；事件循环、状态机、批处理、SQLite 访问层和协议层（HTTP/JSON/SHA-256）是本实现独立编写的。

## 读取走 SQLite 的 B-tree，而不是 SQL 引擎

`GET /feed` 和 `GET /posts/:id` 在 pass 的 SQLite 事务内，读 B-tree 游标（`src/shim.c` `#include` 未修改的 amalgamation）。C++ 侧解码 record 字节并直接写 JSON：没有字节码引擎，没有 `sqlite3_column_*` 调用，也没有经过 SQLite value 的拷贝。

- **Feed：** 从 `posts_created_at_id_idx` 的第一条开始遍历。索引是 `(created_at DESC, id DESC)`，正好是 `ORDER BY created_at DESC, id DESC`，record 里已经带着 `created_at` 和 `id`。然后按 id 升序在 `posts` 里查找这 20 篇，连续 id 落在同一个 leaf 的下一个 cell（`TableMoveto` 的快速路径）。接下来是 `users` 里的作者。like 计数把 20 个 id 排序后沿 `likes_post_id_idx` 做一次 merge 扫描。
- **实时读取：** 每个请求在被服务时从 SQLite 读它的行（规则 5）。只复用预编译语句、游标、KeyInfo 和四个根页号（启动时从 `sqlite_schema` 读取）。`unix-excl` VFS 阻止其他进程打开文件。
- **写入走 SQL：** post 用 `INSERT … RETURNING id, created_at`，like 用 `INSERT … ON CONFLICT DO NOTHING`；只有 `sqlite3_changes` 区分新 like 和重复 like。like 处理器检查 post 是否存在是一次 `posts` 上的 B-tree seek。

## 持久化与批处理

- WAL + `synchronous=NORMAL`，通过 `unix-excl` VFS 打开；WAL 索引在进程内共享，没有 `-shm` 文件。
- 每个事件循环 pass 是一个 SQLite 事务：读共享快照，写入组提交（规则 6）。**COMMIT 返回之前，pass 的任何响应都不会交给内核。** commit 失败时该 pass 的每个响应都变成 500。
- 自动 checkpoint 关闭。COMMIT 后 WAL 帧数达到 4,096（16 MB）时，由独立的 checkpoint 线程用自己的连接执行一次 PASSIVE checkpoint；它不等待读者，事件循环也不等它的 fsync。
- 不使用 `mmap`：读走 SQLite 的页缓存（64 MB）。

## 连接

每个 k6 用户持有一个 keep-alive 连接，所以高分意味着在 `LimitNOFILE=65535` 下数十万个 socket。

- 接受的 socket 是 io_uring **直接描述符**：它们位于 ring 的文件表，不占进程 fd 表。表大小跟随 `RLIMIT_NOFILE` 的硬限制。
- 8 个 ring 各有一个 `SO_REUSEPORT` listener；每个 ring 保持 32 个单发 accept 在途。ring 接近满时关闭自己的 listener，其余 ring 继续接收。
- 每个连接有一个 **multishot receive**，缓冲来自 provided-buffer ring（每 ring 512 × 4 KB），所以空闲连接不持有接收内存。
- 突发之后 multishot receive 会耗尽 buffer 并以 ENOBUFS 结束；这样的连接进入 ring 内的 FIFO 队列，每个 pass 给它们发单发读（`MSG_DONTWAIT`，且每个 ring 不超过 buffer 数）。填满 buffer 的读重新入队，短读或 EAGAIN 则重新装好 multishot receive。
- 每个连接同一时间最多一个发送在途；流水线响应排在它后面。输出缓冲来自池，发送完成后归还。
- 关闭是 `SHUTDOWN` 硬链接到 fixed-file `CLOSE`；每个 `user_data` 里的 generation 号会丢弃复用槽位的完成。

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
- `bench/load.js` 在 **8,000 VUs**（30 s 爬坡，45 s 保持）：**0.00% 失败请求**，p95 3.05 ms，p99 3.63 ms。
- 单核无思考时间循环 `GET /feed`（1,024 连接）：约 40k req/s，20 µs/请求 CPU；`perf` 显示约一半 CPU 在内核 TCP/io_uring 路径。

## 运行

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```
