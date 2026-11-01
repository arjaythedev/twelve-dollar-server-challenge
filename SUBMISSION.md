# Python + C + Granian

Python 3.15.2, Granian 2.8.4, SQLite 3.53.1, orjson 3.13.0 and uv 0.13.6. Dependencies are pinned in src/uv.lock.

Direct HTTP/1.1 with keep-alive, one worker and one request execution thread. One SQLite connection, WAL and synchronous=NORMAL. Each request reads fresh data; each write commits before its response.

Feed and post queries return JSON bytes through our C extension. It uses SQLite's public function API, writes the fixed fields directly and escapes strings in 16-byte SSE2 blocks on x86-64. Feed ordering uses raw timestamps and IDs. Buffers live for one query. SQLite itself is unchanged. Other responses use orjson, with stdlib encoding for escaped lone surrogates.

## Run

From the repository root on Ubuntu 24.04:

```sh
sudo bash src/install.sh
bash src/build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash src/start.sh
```

Use a fresh copy of the official seed. HOST, PORT, SQLITE_PATH and JWT_SECRET configure the server.

For local checks and encoder comparisons, gcc must be on PATH:

```sh
uv run --directory src --locked dev.py check
uv run --directory src --locked dev.py encoding
```
