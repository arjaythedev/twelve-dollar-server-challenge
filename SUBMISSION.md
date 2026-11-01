# Python + Granian

Python 3.15.2, Granian 2.8.4, SQLite 3.53.1 and uv 0.13.6. Dependencies are pinned in src/uv.lock.

Direct HTTP/1.1 with keep-alive, one worker and one request execution thread. One SQLite connection, WAL and synchronous=NORMAL. Each request reads fresh data; each write commits before its response. SQL reads and Python JSON serialization are separate. No custom batching or native extensions yet.

## Run

From the repository root on Ubuntu 24.04:

```sh
sudo bash src/install.sh
bash src/build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=3000 bash src/start.sh
```

Use a fresh copy of the official seed. HOST, PORT, SQLITE_PATH and JWT_SECRET configure the server.
