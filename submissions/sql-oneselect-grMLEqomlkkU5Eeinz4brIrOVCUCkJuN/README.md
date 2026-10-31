# sql-oneselect

The whole API is one SQL statement, [`src/server.sql`](src/server.sql). Incoming HTTP requests are rows
of `http_requests`, a SQLite virtual table, and each reply is a value in the statement's result. Routing,
auth errors, validation, the feed, likes and new posts are all written in that file. The C program
fills the table, runs the statement, and sends the replies.

| | |
|---|---|
| Language | SQL (SQLite 3.54.1), C for the socket loop |
| Framework | none |
| Driver | SQLite 3.54.1 amalgamation, compiled in, pinned by SHA-256, unpatched |
| Nginx or direct | direct, `0.0.0.0:80` |
| Transport | epoll on one thread; a second thread runs WAL checkpoints |

## Request flow

1. The loop reads every ready socket and parses complete requests into a batch of at most 256.
2. It steps the statement. `http_requests` returns one row per request in the batch, then ends.
3. The `CASE` in each row builds the reply as `'<status> <json>'` and `http_reply()` queues it. Likes and
   new posts are written by `write_like()` and `write_post()`, which run prepared INSERTs inside the
   statement's transaction.
4. When the statement finishes, SQLite's autocommit commits its writes. One statement run is one
   transaction, so a batch is group-committed without extra code.
5. The replies are sent after the commit. If the commit fails, every request in the batch gets a 500.

`http_requests` is an eponymous-only virtual table. It is registered on the connection with
`sqlite3_create_module`, so no `CREATE` statement runs and the schema is unchanged (rule 3).

## Rules notes

- Caching: nothing is kept between statement runs except prepared statements and the HMAC key pads
  derived from `JWT_SECRET`.
- Durability: WAL with `synchronous=NORMAL`, and replies are sent only after the commit.
- JWT: every request with a token gets a full HS256 check: signature, `alg`, `exp` (required, in the
  future), then `sub` and `username`.
- Connections: with `LimitNOFILE=65535`, the server closes the longest-idle connections when it gets
  within 1,024 descriptors of the limit. k6 users idle 3 to 15 s between requests, so a closed connection
  is usually idle when the FIN arrives. This is untested under load.

## Performance choices

- The statement is prepared once. The virtual table passes pointers into the batch to SQLite instead of
  copying.
- One transaction per batch.
- The loop's connection has `wal_autocheckpoint=0`. A second connection on its own thread runs a passive
  checkpoint every 200 ms, so fsync does not block the loop.
- Idle connections hold no read or write buffers.

## Testing

CI runs `bash test/run.sh submissions/sql-oneselect-grMLEqomlkkU5Eeinz4brIrOVCUCkJuN`.

`./build.sh harness && ./test-harness.sh` runs the repo's `test/test.sh` (42/42) without a network. It
replaces the script's curl calls with `bin/harness`, which feeds the same HTTP bytes through the parser,
the virtual table and `server.sql`. `bin/harness src/server.sql batch < requests.http` answers a file of
requests in a single statement run.

`bin/server` itself has not been run yet, and there has been no load test. CI will be its first run.
