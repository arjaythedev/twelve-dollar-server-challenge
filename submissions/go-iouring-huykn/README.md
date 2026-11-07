# go-iouring-huykn

Go 1.27.3, SQLite 3.54.1 compiled directly in C with cgo, and
`github.com/godzie44/go-uring` pinned to `d16a9e597639`.
Serves directly on `HOST:PORT`, without Nginx; keep-alive sockets have no idle timeout.
Set `HOST=0.0.0.0 PORT=80` for the challenge (defaults: `127.0.0.1:3000`).
Requires Linux with io_uring enabled.

- SQLite B-tree reads, prepared writes, schema checks, SQL fallbacks, and background
  WAL checkpoints follow `go-iouring-trungdlp` and `rust-iouring-kensac`.
- A single event-loop thread owns SQLite. WAL uses `synchronous=NORMAL`;
  up to 256 ready completions share a commit before any new response is sent.
  Batching adds no fixed collection delay; commit failures replace staged responses.
- No JSON marshal/unmarshal. `scan.go` validates raw bytes with a visitor, skips unknown values,
  and decodes selected escaped strings. `bytes.go` appends output bytes directly.
  C renders read responses directly into spare byte capacity, with a Go fallback.
  Post objects use the reusable `postJSON` byte buffer, then append into responses.
  `like_count` stays last; `likeOffset` allows replacing only its numeric tail.
- Buffers are rebuilt from SQLite for every request. No response, row, like-count,
  or JWT verification cache; the fixed challenge schema stays unchanged.
- Networking uses go-uring's accept/recv/send operations, including partial sends,
  fragmented requests, and HTTP pipelining. No custom ring implementation.
- Go structs pass `fieldalignment` from `golang.org/x/tools` v0.52.0;
  tests guard their 64-bit sizes.

Build and check from the repository root on Linux:

```sh
bash submissions/go-iouring-huykn/install.sh # once, as root
bash test/run.sh submissions/go-iouring-huykn
```

Benchmarks check zero Go allocations after warm-up in application handlers
(including SQLite), plus byte construction. Use a disposable seed copy:

```sh
cd submissions/go-iouring-huykn
cp ../../seed/feed.db /tmp/huykn-bench.db
BENCH_DB=/tmp/huykn-bench.db go test -run '^$' -bench . -benchmem -benchtime=100x
```
