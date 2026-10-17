# Go + raw epoll

A Go server on one epoll loop, with SQLite called through a small cgo binding. It serves the internet directly on
`0.0.0.0:80`, without Nginx.

Go 1.27.2 comes from the official tarball, and `install.sh` verifies its checksum. `build.sh` downloads the SQLite
3.53.4 source from sqlite.org, verifies its checksum and compiles it with Ubuntu's GCC into a profile guided static
library. There is no HTTP framework and no `database/sql`. The only dependencies are the Go standard library and
SQLite.

## Running it

```bash
sudo bash install.sh   # apt: build-essential, ca-certificates, curl. Go into /usr/local/go
bash build.sh          # profile guided SQLite into .deps/, the server into bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

`build.sh` reads the challenge's `schema.sql` from the repository root for its SQLite training run, so it must run
from a checkout of the repository.

## Design, and why

One goroutine runs an epoll loop that accepts, reads, handles and writes every connection. Go's `net/http` keeps a
goroutine and read and write buffers for each connection for as long as it is open, and another Go entry measured it
at 990 MB RSS for 50,000 users, half the box. Here an idle connection is a small struct with no buffers. Reads land
in one shared 64 KiB buffer, and only an incomplete request is copied into its connection. RSS stays under 20 MiB at
60,000 connections.

SQLite runs inline on the loop. Every query is an index lookup that takes microseconds, and on one vCPU handing it
to another thread would only add context switches. Only the loop uses the connection, so SQLite is compiled without
its internal mutexes (`SQLITE_THREADSAFE=0`) and opened in exclusive locking mode. Exclusive mode takes the file
locks once and keeps the WAL index in process memory instead of a shared memory file. This cut the server's CPU by
about 1.4 µs per request. The journal is WAL with `synchronous=NORMAL`, as rule 6 allows.

Writes use group commit. The first write in an epoll batch opens a transaction, and every response the batch
produces is held in one buffer. After the batch the transaction commits, and only then are the responses written to
their sockets. No write is acknowledged before it is committed, and no read is answered with data that could still
roll back. If the commit fails, every response in the batch becomes a 500, reads included. Group commit cut about
1.7 µs per request.

The feed and single post queries are one cgo call each. A small C helper steps every row and packs the columns into
one buffer, and Go writes the JSON from it with an escaper that copies unescaped runs whole. Reading the feed's
columns one cgo call at a time took about 200 calls, and having SQLite build the JSON with `json_object` was slower
than writing it in Go. Packing rows cut about 2.6 µs per request compared with SQLite building the JSON.

SQLite would normally checkpoint inside the `COMMIT` that takes the WAL past 1,000 pages, so that batch's responses
would wait for the page copies and fsyncs. A WAL hook turns that off, and the loop checkpoints at the same threshold
after the batch's responses have been written. This moves the work rather than reducing it.

The SQLite library is profile guided. `build.sh` compiles an instrumented SQLite, runs `pgo/train.c` against a
throwaway database of synthetic rows using the server's statements in roughly the load test's mix, then compiles
SQLite again from the recorded profile. No challenge data is used. The feed query runs about 7% faster in process.
End to end that is about 0.35 µs per request, which is below the noise of the saturation test.

Every token is verified on every request: HS256 only, a constant time signature comparison and `exp` and `nbf`
checked. Nothing is cached between requests.

Idle keep-alive connections are never closed. At the descriptor limit the server stops watching the listener
rather than spinning on a failing `accept`, and starts again when a connection closes.

## Measurements

These figures are local. They were taken on an Apple M4 Max laptop in Docker Desktop, with the server pinned to one
CPU with 2 GiB, no swap and 65,535 descriptors, and the load generators on other CPUs. The official score comes from
the droplet.

With wrk sending the `bench/load.js` request mix with no think time over 64 keep-alive connections, the server handles
about 78,000 requests per second on its one core and uses 11.3 µs of CPU per request. Each change above was measured
against the build before it in interleaved trials.

With k6 running the unchanged `bench/load.js` at 40,000 users, the server uses 14% of a core, with a p99 of 5 ms and
17 MiB RSS. Higher user counts are not a reliable local test, because k6 itself becomes the bottleneck on this
machine (17 GiB of memory at 60,000 users).

## Testing

`test/run.sh` passes all 42 checks, on both x86_64 and arm64. `go test` covers the HTTP parser, token verification,
request validation and JSON encoding. The scripts in `tests/` run on Linux from the repository root after `build.sh`,
with Go, `curl`, `jq`, `openssl` and `sqlite3` installed. They check that every acknowledged write survives
`SIGKILL`, that a failed commit acknowledges nothing, that the WAL stays bounded under thousands of writes and that
the server neither spins nor stalls at its descriptor limit.

## Behaviour notes

- Bodies are trimmed with JavaScript's `String.prototype.trim()` whitespace set, and their length is counted in
  Unicode code points, as SQLite's `length()` does.
- Pipelined requests are answered in order. A malformed request, headers over 16 KiB or a body over 16 KiB get a
  400 and the connection is closed. Chunked request bodies are not supported.
- Exclusive locking means no other process can open the database while the server runs.

## Licence

MIT, under the repository's [licence](../../LICENSE).
