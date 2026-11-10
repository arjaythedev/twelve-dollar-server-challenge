# Java: io_uring, a write queue, and SQLite's B-tree cursors, through FFM

A Java port of [goose-iouring-davidhsv](../goose-iouring-davidhsv) (#384), which ports the design of [rust-iouring-kensac](../rust-iouring-kensac) (#264): one thread serves every connection through io_uring and every request through one SQLite transaction per pass. The question here is what that design costs in Java on HotSpot, with every decision made in Java and C kept to what the JVM cannot do by itself, measured against the entries it comes from.

| | |
|---|---|
| Language | Java 27, OpenJDK 27.0.1 (the GA build from jdk.java.net), fetched by `install.sh` and checked against its published sha256 |
| Framework | none: the event loop, HTTP/1.1, JSON and HS256 (SHA-256 and HMAC included) are written in Java here |
| Native calls | `java.lang.foreign` (FFM, final since Java 22) into `libfeed.so`, built from `net.c` and `db.c` |
| io_uring | raw system calls against the kernel's `<linux/io_uring.h>`; no liburing |
| SQLite | 3.54.1 amalgamation (sha256-checked), unmodified, compiled into `libfeed.so`; opened through a small VFS of its own over `unix-excl` |
| C compiler | gcc 13 (Ubuntu 24.04 `build-essential`), `-O3`, link-time and profile-guided optimization |
| JVM | HotSpot, Serial GC, 64 MB heap |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Needs | Linux 6.1+ (io_uring with `DEFER_TASKRUN`, multishot receive, provided-buffer rings, direct descriptors). No epoll fallback, so it does not start where io_uring is blocked (Docker's default seccomp profile) |

__REST__
