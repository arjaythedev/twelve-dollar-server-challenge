# Go Fiber submission

This submission runs behind the challenge's Nginx configuration on `127.0.0.1:3000`. HTTP keep-alive connections have a 120-second idle timeout.

The pinned stack is Go 1.27.3, Fiber v2.52.16, `github.com/mattn/go-sqlite3` v1.14.53, and `github.com/golang-jwt/jwt/v5` v5.3.1. SQLite 3.54.1 compiles from the driver's bundled source with CGO.

## Build and run

Run `bash install.sh` as root on Ubuntu 24.04 to install the compiler and checksum-verified Go toolchain. Run `bash build.sh` as a normal user.

Start the server with the challenge environment variables:

```bash
SQLITE_PATH=/path/to/feed.db \
JWT_SECRET=twelve-dollar-challenge \
HOST=127.0.0.1 PORT=3000 bash start.sh
```

`SQLITE_PATH` must name an existing database. `JWT_SECRET` is required. `HOST` defaults to `127.0.0.1`, and `PORT` defaults to `3000`.

From the repository root, run the official checks against a fresh seed copy:

```bash
bash test/run.sh submissions/go-fiber-yeseolee
```

## Database behavior

One SQLite connection serializes database operations. WAL mode with `synchronous=NORMAL` provides the required durability setting. Foreign keys and a five-second busy timeout apply to every connection.

Each request reads current database rows. The feed uses the existing timestamp and ID index, and like counts use the existing post index. No response, query result, or token verification is cached. Each write uses an explicit transaction and commits before the HTTP success response.

Ordered response structs preserve the required JSON field order. Request validation counts Unicode characters after trimming JavaScript whitespace. Each authenticated request verifies the HS256 signature and expiration before validating its application claims.
