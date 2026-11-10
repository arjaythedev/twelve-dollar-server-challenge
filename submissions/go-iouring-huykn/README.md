# Go + io_uring + SQLite

A direct HTTP server using a local io_uring binding, per-request HS256
verification, an independent indexed database read plan, and SQL writes batched
before response sends. JSON output is built directly in Go.

| Component | Configuration |
|---|---|
| Runtime | Go 1.27.3, cgo, one locked event-loop thread |
| Networking | Direct descriptors over multiple rings; shared provided buffers |
| Database | Stock SQLite 3.54.1, downloaded and checksum-verified at build time |
| HTTP / JSON / JWT | Application code and standard-library cryptography |
| Deployment | Direct on `HOST:PORT`; `0.0.0.0:80` for the benchmark |
| Platform | Linux with sparse direct descriptors, multishot receives and deferred task work |

## Database design

`store.c` owns one connection, prepared SQL operations, transaction state, and a
bounded result arena. Its independent read plan is in `btree_reader.h`, compiled
after the pinned, unmodified SQLite amalgamation. `read_plan.h` isolates the
private SQLite types from the rest of the server.

SQLite initializes explicitly at connection startup. Shared-cache support,
progress callbacks, and tracing are compiled out; connections use private caches.

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

The loop wakes on the first completion on ring 0, then drains ready completions.
An idle wait lasts at most 1 ms before polling the other rings. Each pass stages
up to 2,048 receive answers without waiting for a full batch.
Writes and indexed reads start a deferred transaction on
demand. Later requests in the same pass can see
its writes. Responses remain staged until commit succeeds; commit failure turns
all staged responses into errors. Implicit SQLite rollback is tracked so lost
writes cannot be acknowledged as successful.

The code baseline is PR369 (`e7c9291`, identical submission source to `4d85716`),
which reached 276k VUs on x86. The new checkpoint patch is unscored.
WAL uses `synchronous=NORMAL`. PR369's trigger remains 8,192 WAL frames, with
2,048-frame retry spacing. A nice-5 worker copies and pre-syncs the bulk while
writes continue, then copies the tail while POSTs wait for at most 250 ms.
Other connections' reads continue; held pipelines retain ordering and EOF.
Completion is checked between transactions using the existing 1 ms bounded wait.
At 65,536 frames, an idle worker permits a restart after committed sends are
submitted. Long-lived readers can defer restart. Worker startup failure retains
SQLite's automatic 8,192-page checkpoint fallback. Both connections use 64 MiB
growth chunks with native `posix_fallocate` enabled.

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
Accepted sockets occupy a sparse direct-descriptor table per ring. The server
adds rings before each table fills (up to 16), without retiring idle keep-alives.
Eight `SO_REUSEPORT` listeners have up to 512 accepts in flight. Each idle
connection uses a 24-byte record and no private receive/response buffers.

Each ring shares 1,024 receive buffers of 2 KiB. Multishot receives select a
buffer when data arrives; buffer exhaustion falls back to bounded single reads.
Fragmented requests retain only their incomplete bytes; response buffers return
to bounded pools after sends complete. Generation tags reject stale completions.
Pipelining and partial sends remain supported, with no keep-alive timeout.
Go GC uses a 512 MiB soft memory limit; C SQLite and mmap memory are outside it.

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
  -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DHAVE_POSIX_FALLOCATE=1 \
  -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_TRACE -DSQLITE_OMIT_AUTOINIT \
  testdata/store_contract.c sqlite_engine.c checkpoint.c -lm -lpthread -o /tmp/huykn-store-test
cp ../../seed/feed.db /tmp/huykn-contract.db
/tmp/huykn-store-test /tmp/huykn-contract.db
```


## Credits

The local binding (`uring.go`) and multi-ring connection loop (`server.go`)
are adapted from [go-iouring-trungdlp](../go-iouring-trungdlp/), including direct
descriptors, provided buffers, receive recovery and response pools. Some
application/response, validation, and startup helpers also derive from it.
The port retains this entry's database and commit handling and adds submission
accounting, cleanup and ownership tests. There are no third-party Go dependencies.
The byte-oriented JSON visitor
and pass-level commit design were also informed by
[rust-iouring-kensac](../rust-iouring-kensac/).

The database adapter, indexed read-plan module, sparse like-index merge, result
ownership, and per-handle checkpoint lifecycle are implemented in this entry.
