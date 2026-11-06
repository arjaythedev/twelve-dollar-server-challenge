# Goose, io_uring, and SQLite

This server uses Goose and C. It serves HTTP directly without Nginx.

| Item | Implementation |
| --- | --- |
| Language | [Goose 2026.3](https://github.com/aardappel/goose/releases/tag/v2026.3), runtime ID `4d3da949e563`, and GNU C11 |
| Compiler | GCC 13 |
| Framework | None. The server uses Linux io_uring system calls. |
| Database | SQLite 3.54.1, compiled into the server |
| SQLite binding | SQLite C API and internal B-tree functions |

## Why Goose

I selected Goose to examine its data layout and ownership model.
The goal is to decrease connection storage and copies of request data.
Goose compiles its source to C.
The C code uses the transport and database code from the reference.
I used GPT-6.1 Terra with maximum reasoning effort during development.

The reference is [Pavlyhalim's C submission](https://github.com/arjaythedev/twelve-dollar-server-challenge/tree/7387a313d58041c76e0fe8134825ef335eeafd0d/submissions/c-iouring-pavlyhalim).
That submission has an official score of 295,000 users.
I reached 300,000 during testing.

## Optimizations

### Connection data

Goose uses a 16-byte record for each connection slot.
The reference uses a 40-byte record.

A Goose queue handle uses 8 bytes.
Each handle has a slot index and a generation number.
The generation number prevents events for closed connections from changing new connections.

Deferred response records store metadata until the database commit.
An error record uses 11 bytes.
A data record uses 22 bytes.
The C code stores the response bytes in its own memory.
It keeps those bytes until the last send completes.

### Request data

Goose can use a slice of the native receive buffer when the buffer has the full request.
This can decrease copies of request data.
The C bridge does not keep a borrowed Goose slice after a call returns.

Goose uses pools for request fragments and delayed input.
The pools can decrease memory allocation for new requests.
The pools keep their capacity between requests.

The loop uses a limit of 2,048 attempts for new input and the `ready` queue.
The loop keeps up to 1,024 attempts for the `ready` queue.

### Network

The server uses io_uring direct descriptors and eight listener sockets.
The server adds rings when the connection count increases.
Multishot polls wait for data on idle connections.
Each ring issues no more reads than the number of its receive buffers.
The server uses these buffers for all connections on the ring.

### SQLite and authentication

The server opens the SQLite file at `SQLITE_PATH`.
It does not change the supplied schema.
The server reads the necessary application data from SQLite for each request.
The SQLite page cache is 8 MiB by default.
The server uses prepared statements for more than one request.

Reads use SQLite B-tree cursors when possible.
The server uses SQL when a B-tree plan cannot complete.
Writes use prepared SQL statements.

The server can put writes from more than one request in one transaction.
It commits each transaction before it sends the success responses.
The event loop uses one thread.
A second thread runs SQLite checkpoints.
The server uses WAL with `synchronous=NORMAL` for requests.
It does not keep query results or token verification results across requests.

For each authenticated request, the server does a check of the token signature and expiry.
It accepts only HS256 tokens.
The loop reads the time at the start of each batch.
The expiry check uses that time.
The token parser accepts a decoded username of at most 255 bytes, as in the reference.

## Compiler optimizations

The build uses link-time optimization across the Goose-generated code and the C server.

## Google Cloud simulation

The native load generator reported a pass with 300,000 virtual users and a 300.001-second hold.
For the whole run, p95 latency was 5.99 ms, p99 latency was 17.03 ms, and the error rate was 0%.
The server used Linux kernel `6.8.0-1071-gcp`.

The files in `reference/pavly-c/src/` are unchanged from the cited reference.
This submission uses the challenge's MIT license.
