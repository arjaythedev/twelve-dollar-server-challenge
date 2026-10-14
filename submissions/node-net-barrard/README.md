# Node.js, no framework

- **Language/runtime:** Node.js v24.21.0 (official tarball)
- **Framework:** none. HTTP/1.1 is parsed by hand on `node:net` sockets, with a hand-rolled router
- **SQLite driver:** `node:sqlite` (built into Node, `DatabaseSync`)
- **JWT:** hand-rolled HS256 check with `node:crypto` (`createHmac` + `timingSafeEqual`)
- **Dependencies:** zero. No `package.json`, no `node_modules`
- **Nginx or direct:** direct (`0.0.0.0:80`). Node holds the ~15k idle keep-alive connections cheaply, so we skip the CPU Nginx would take

## Optimizations

- **No `node:http`.** Once SQLite was fast, most CPU went to Node's HTTP layer (request/response objects,
  streams, events). The server reads raw bytes from each TCP socket, parses the request line and the few
  headers it needs (`Content-Length`, `Authorization`, `Connection`, `Expect`), and writes each response
  as one string. Pipelined requests are answered in order, in one write. Measured under the same load:
  about 18% less CPU per request and half the memory. It speaks keep-alive, HTTP/1.0, `Connection: close`
  and `Expect: 100-continue`, and closes the connection on malformed or oversized requests (400/413/431).
  Chunked request bodies get a 411: clients send `Content-Length` for small JSON bodies.
- Every statement is prepared once at startup and reused.
- SQLite builds the whole response body for `GET /feed` and `GET /posts/:id`
  (`json_object` / `json_group_array`, keys in the spec's order), so one string crosses into JS
  instead of row objects. That's about 10% less CPU per request, mostly from less allocation and GC.
- Statements return rows as arrays (`setReturnArrays`), not objects.
- Likes take one statement on the happy path: `INSERT ... SELECT ... FROM posts WHERE id = ? ON CONFLICT DO NOTHING`
  inserts only when the post exists. The existence check runs only when nothing was inserted.
- PRAGMAs: WAL, `synchronous=NORMAL`, 64 MB page cache, 256 MB `mmap`.
- Idle connections are kept for 70 s, longer than any k6 think time, so users keep their connection instead of reconnecting.
