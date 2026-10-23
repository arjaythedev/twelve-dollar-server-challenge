# Trux HTTP

[Trux](https://github.com/thea1lab/trux) is an [A1 Lab](https://github.com/thea1lab) project. This server is the feed API. One process, one worker, SQLite in-process. The HTTP runtime speaks HTTP/1.1 with keep-alive and listens on `HOST` and `PORT`. There is no response cache and no JWT cache. Writes go to the seed schema with WAL and `synchronous=NORMAL`.

The challenge secret is 23 bytes. This server needs the Trux compiler commit `aefc5e2a1f0ebb24fa7476ba703377b3cf9e8357`, which zero-pads a short HS256 key and keeps returned strings and SQLite handles alive. `install.sh` builds that commit.

## Pins

| | |
|---|---|
| Compiler | [Trux `aefc5e2a1f0ebb24fa7476ba703377b3cf9e8357`](https://github.com/thea1lab/trux/commit/aefc5e2a1f0ebb24fa7476ba703377b3cf9e8357) |
| Go | 1.26.3 |
| libjwt | 3.6.1, static |
| liburing, jansson, OpenSSL | Ubuntu 24.04 packages |
| SQLite | the copy compiled into that Trux runtime |

## Run

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 bash start.sh
```

`test/run.sh submissions/trux-http-claudioscheer` is the check CI runs after `install.sh`. Direct serving, no Nginx.
