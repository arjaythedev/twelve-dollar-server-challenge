# Go + io_uring + SQLite, served directly

A single locked OS thread runs a Go io_uring event loop, HTTP request framing,
byte-oriented JSON parsing, HS256 verification, and SQLite access through cgo.
The main work in this submission is the library-based transport, completion-driven
batching, JSON byte scanner, and reusable response buffers.

| Component | Version / configuration |
|---|---|
| Language | Go 1.27.3; cgo with a C compiler |
| Networking | `github.com/godzie44/go-uring`, pinned to `d16a9e597639` |
| SQLite | Stock 3.54.1 amalgamation, checksum-verified by `build.sh` |
| HTTP / JSON / JWT | Application code; no web framework or `encoding/json` |
| Nginx or direct | **Direct**, on `HOST:PORT`; use `0.0.0.0:80` on the benchmark server |
| Platform | Linux with io_uring enabled |

## Request path

- **Transport.** `server.go` uses the library's accept, receive, and send operations.
  A connection owns its input/output buffers until the operation completes.
  Fragmented requests, HTTP pipelining, and partial sends are handled explicitly.
  Keep-alive sockets have no idle timeout.
- **Scheduling.** Each pass drains up to 256 already-ready completions, prepares
  responses, commits the database transaction, then queues the new response sends.
  There is no fixed collection delay; batching follows available completions.
- **JSON input.** `scan.go` validates the entire input through a byte visitor,
  skips unknown values, and decodes selected escaped strings. Request handlers and
  JWT claim parsing use this scanner without unmarshalling into object trees.
- **JSON output.** `bytes.go` appends escaped strings and numbers directly.
  A reusable `postJSON` buffer places `like_count` at the object's tail, so writing
  that number does not move the other fields. Feed/post reads normally use the C
  renderer, with Go byte construction as the fallback.
- **Database.** Feed and post reads use SQLite's B-tree cursor operations; writes
  use prepared SQL. Schema checks guard the read plans, and SQL fallbacks handle
  unexpected layouts. Database operations cross into C per request.
- **Runtime.** `GOMAXPROCS=1`, `runtime.LockOSThread`, and
  `GODEBUG=asyncpreemptoff=1`. Struct layouts were checked with `fieldalignment`
  from `golang.org/x/tools` v0.52.0 and have size tests on 64-bit targets.

## Durability and request isolation

WAL uses `synchronous=NORMAL`. Responses produced by a pass wait for its commit;
if the commit fails, staged responses become errors before any of their bytes are
queued for sending. WAL checkpoints run on a separate connection in a background
thread, with the write-hold state checked between response batches.

Every request reads its data from SQLite and every authenticated request verifies
its JWT signature and required claims. Prepared statements and SQLite's page cache
are reused; response data and verified tokens are not cached across requests.
Reusable byte buffers are rebuilt for the request being served. SQLite's source
and the challenge schema remain unchanged.

## Build and check

From the repository root on Linux:

```sh
sudo bash submissions/go-iouring-huykn/install.sh
bash test/run.sh submissions/go-iouring-huykn
```

`start.sh` runs in the foreground. Configuration is through `SQLITE_PATH`,
`JWT_SECRET`, `HOST`, and `PORT`; the address defaults to `127.0.0.1:3000`.

Tests cover JSON validation and escapes, JWT claims, HTTP framing and pipelining,
response bytes, numeric rendering, SQL fallback, and struct sizes. Use a disposable
seed copy for the database-backed tests and handler benchmarks:

```sh
cd submissions/go-iouring-huykn
cp ../../seed/feed.db /tmp/huykn-bench.db
BENCH_DB=/tmp/huykn-bench.db go test ./...
BENCH_DB=/tmp/huykn-bench.db go test -run '^$' -bench . -benchmem -benchtime=100x
```

The overflow fallback fixture also needs the `sqlite3` CLI. Tests without their
required fixture are skipped. Benchmarks report Go allocations; C allocations and
kernel socket memory are outside that count. An official score requires the
organizer's load test and five-minute hold on the benchmark server.

## Credits

The SQLite layer (`shim.c`, `shim.h`, `db.go`) and portions of the handlers,
HTTP, JWT, and startup code reuse and adapt
[go-iouring-trungdlp #343](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/343).
That layer includes credited helpers from
[rust-iouring-kensac #264](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/264)
(checkpoint/write-hold logic and index seek) and the page-cache truncate shortcut
idea from [c-iouring-pavlyhalim #245](https://github.com/arjaythedev/twelve-dollar-server-challenge/pull/245).
Networking uses [godzie44/go-uring](https://github.com/godzie44/go-uring).
