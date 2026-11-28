# rust-iouring-viniciusvidaldev

- **Language:** Rust 1.99.1
- **Framework:** none, raw io_uring through direct syscalls
- **SQLite:** 3.54.1, vendored amalgamation compiled statically, called through hand-written FFI
- **Crates:** jsonwebtoken 12.0.0, serde 1.0.229, serde_json 1.0.154, cc 1.11.3 (build)
- **Serving:** direct on `0.0.0.0:80`, no Nginx

## Features

- Single thread, no async runtime
- 5 io_uring rings, connections spread across them, one ring blocks and the others wake it through eventfd
- Multishot accept into fixed file slots, 65,535 per ring
- Multishot recv with a provided buffer ring per ring (16,384 × 1 KiB)
- ENOBUFS handling: starved connections are queued and rearmed after each drain, capped by the buffers recycled
- Completion queue sized so it can't overflow
- Listen backlog of 4,096
- Hand-written HTTP/1.1 parser, keep-alive and pipelined requests
- SQLite built with `THREADSAFE=0`, `DEFAULT_MEMSTATUS=0`, `OMIT_SHARED_CACHE`, `OMIT_LOAD_EXTENSION`, among others
- WAL with `synchronous=NORMAL`
- Prepared statements reused for every request, never re-prepared
- Every token's HS256 signature verified on every request
- Workaround for Ubuntu 6.8 kernels that reject a zeroed buffer ring registration (Launchpad #2162843)
