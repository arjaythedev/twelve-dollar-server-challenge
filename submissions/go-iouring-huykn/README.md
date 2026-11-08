# Go + io_uring + SQLite

A direct HTTP server using `github.com/godzie44/go-uring`, per-request HS256
verification, an independent indexed database read plan, and SQL writes batched
before response sends. JSON output is built directly in Go.

| Component | Configuration |
|---|---|
| Runtime | Go 1.27.3, cgo, one locked event-loop thread |
| Networking | `github.com/godzie44/go-uring` at `d16a9e597639` |
| Database | Stock SQLite 3.54.1, downloaded and checksum-verified at build time |
| HTTP / JSON / JWT | Application code and standard-library cryptography |
| Deployment | Direct on `HOST:PORT`; `0.0.0.0:80` for the benchmark |
| Platform | Linux with io_uring enabled |

## Database design

`store.c` owns one connection, prepared SQL operations, transaction state, and a
bounded result arena. Its independent read plan is in `btree_reader.h`, compiled
after the pinned, unmodified SQLite amalgamation. `read_plan.h` isolates the
private SQLite types from the rest of the server.

The read plan uses SQLite's read-only B-tree cursor operations. It does not
modify SQLite pages or pager state:

1. Read the first 20 keys from the descending creation-time index.
2. Track ID order while reading the index. Already descending IDs can be
   traversed in reverse without a sorting pass; otherwise sort lookup tasks by
   rowid while retaining output slots. Arbitrary timestamp/ID order is supported.
3. Resolve authors in lookup order. Consecutive equal author IDs reuse one
   lookup and copied text span within that request only.
4. Merge ordered requested post IDs with the like index, seeking across gaps
   instead of scanning unrelated ranges or allocating a dense counter array.

Record fields are bounded views over SQLite payload bytes. Overflow pages are
fetched only when a requested field is not completely local. Text needed by Go
is copied into the handle's 64 KiB arena before returning. Go borrows a view of
those records and serializes their spans directly, without building a second
20-row array. All private cursors close before commit or rollback; none retain
a page or snapshot across batches. The cgo adapter passes an opaque integer
handle for C-allocated stores, with explicit open/close ownership. It never
encodes a Go pointer; input Go buffers retain normal cgo pointer checks.

Schema, index order, collation, encoding, record types, and schema-cookie checks
guard the plan. Unsupported cases run the reference SQL query. A missing author
therefore follows the SQL JOIN's visibility rules. Result overflow returns an
error rather than partial JSON.

Writes use public prepared SQL. Post creation inserts and then reads the stored
ID and timestamp on the same connection and within the same transaction. This
avoids the ephemeral result table used by `RETURNING`. The input body is borrowed
only during the cgo call; the statement is reset and bindings cleared on every
exit before Go regains control. Repeated likes execute a fresh SQL existence
query and skip a write transaction if already present.

## Transactions and checkpoints

The loop drains at most 256 ready completions per pass. Writes and indexed reads
start a deferred transaction on demand. Later requests in the same pass can see
its writes. Responses remain staged until commit succeeds; commit failure turns
all staged responses into errors. Implicit SQLite rollback is tracked so lost
writes cannot be acknowledged as successful.

WAL uses `synchronous=NORMAL`. A new per-handle worker runs passive checkpoints
on its own connection. Notifications coalesce and start at 8,192 WAL frames,
with further notifications at coarse growth intervals. At 65,536 frames the
request thread waits for the worker, then attempts a WAL restart outside any
transaction. Long-lived readers may defer a restart; the normal server has no
cross-batch read cursors. Worker startup failure retains SQLite's automatic
checkpoint fallback at 8,192 pages.

The default is SQLite's built-in `unix-excl` VFS: one process owns the database
and SQLite keeps the WAL index in heap memory. Other processes cannot open the
live database until this server exits. See [SQLite's VFS documentation](https://sqlite.org/vfs.html).
The page cache is SQLite's default implementation, limited to 256 MiB on the
request connection and 2 MiB on the checkpoint connection. There is no custom
page cache or VFS. The schema and SQLite source are unchanged.

There are no response, query-result or verified-token caches. Each request
reads its database state, and every authenticated request verifies its signature
and claims. Author reuse ends with each read operation.

Diagnostic switches (also used for differential tests) are `HKN_SQL_READS=1` to
force reference SQL reads, `HKN_VFS=unix` to use SQLite's default Unix VFS, and
`HKN_CHECKPOINT=inline` for automatic inline checkpoints. These change only the
server's own database configuration; none detect benchmark traffic.

## Build and validation

From the repository root on Linux:

```sh
sudo bash submissions/go-iouring-huykn/install.sh
bash test/run.sh submissions/go-iouring-huykn
```

`start.sh` stays in the foreground. Runtime configuration uses `SQLITE_PATH`,
`JWT_SECRET`, `HOST`, and `PORT`; the default address is `127.0.0.1:3000`.
Connections retain receive/send buffers until io_uring completion. Fragmented
requests, pipelining and partial sends are supported, with no keep-alive timeout.

Use disposable seed copies for database-backed tests:

```sh
cd submissions/go-iouring-huykn
cp ../../seed/feed.db /tmp/huykn-unit.db
BENCH_DB=/tmp/huykn-unit.db go test ./...
go vet ./...
cp ../../seed/feed.db /tmp/huykn-bench.db
BENCH_DB=/tmp/huykn-bench.db go test -run '^$' -bench BenchmarkDatabase -benchmem -benchtime=10000x
```

Fixtures compare indexed and SQL reads for sparse IDs, mixed timestamps/authors,
missing authors, schema/encoding fallbacks, and overflow pages. Tests also cover
all SQLite integer widths (including signed limits), 300 randomized SQL/JSON
round trips, JWT claims, strict base64url, HTTP framing, response staging, and
buffer ownership. JSON escaping uses bounded unaligned word loads on Linux
amd64/arm64, including overlapping words for short fields. Alignment and
guard-page tests cover short tails and escaping characters; no load may extend
past its input slice. Fixture tests need `sqlite3`; seed tests
need `BENCH_DB` and otherwise skip.

The C contracts additionally verify fast-plan activation, transaction visibility,
commit denial, implicit rollback, fresh author reads, checkpoint snapshot
isolation, the WAL growth barrier, and schema-cookie fallback:

```sh
cc -O1 -DSQLITE_DEBUG -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 \
  -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 \
  testdata/store_contract.c sqlite_engine.c checkpoint.c -lm -lpthread -o /tmp/huykn-store-test
cp ../../seed/feed.db /tmp/huykn-contract.db
/tmp/huykn-store-test /tmp/huykn-contract.db
```

[BENCHMARKS.md](BENCHMARKS.md) records the local comparison and its limits.
These microbenchmarks are not the organizer's full-server score.

## Credits

Some application/response, validation, and startup helpers derive from
[go-iouring-trungdlp](../go-iouring-trungdlp/). The byte-oriented JSON visitor
and pass-level commit design were also informed by
[rust-iouring-kensac](../rust-iouring-kensac/). Networking uses
[godzie44/go-uring](https://github.com/godzie44/go-uring).

The database adapter, indexed read-plan module, sparse like-index merge, result
ownership, and per-handle checkpoint lifecycle are implemented in this entry.
