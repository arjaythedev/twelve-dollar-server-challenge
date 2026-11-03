# Crystal / Kemal

Feed API for the [$12 Server Challenge](../../README.md), serving **directly over HTTP without Nginx**.
Set `HOST=0.0.0.0` and `PORT=80` for the benchmark. The process honors `HOST`, `PORT`,
`SQLITE_PATH`, and `JWT_SECRET` as defined in [SPEC.md](../../SPEC.md).

| Component | Version |
| --- | --- |
| Crystal | 1.21.1 |
| Kemal | 1.15.0 |
| crystal-sqlite3 | 0.24.0 |
| crystal-db | 0.15.0 |
| SQLite | Ubuntu 24.04 system library |

## Build and run

On Ubuntu 24.04 x86_64, install the pinned compiler and build dependencies once as root:

```sh
sudo bash install.sh
```

Build as a normal user. `--production` installs the exact versions in `shard.lock` and
excludes development dependencies; the executable is compiled in release mode.

```sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=your-secret HOST=127.0.0.1 PORT=3000 bash start.sh
```

The server runs in the foreground. The challenge service supplies permission to bind port 80.
Run the existing correctness suite from the repository root:

```sh
bash test/run.sh submissions/crystal-kemal-krthr
```

## Performance choices

- One process, one execution context, and one SQLite connection avoid pool overhead and
  contention on the single CPU. Crystal fibers handle concurrent keep-alive connections.
- Prepared statements reuse query plans; every request still reads current data from SQLite.
- WAL with `synchronous=NORMAL` commits each write before responding. SQLite gets a 64 MiB
  page cache and up to 1 GiB of memory-mapped reads.
- The feed uses the existing ordering index and counts likes through the existing post index.
  The schema is unchanged.
- First-time likes use one conditional insert; repeated likes check post existence only when needed.
- Kemal runs in production with request logging and static file handling disabled.
- JWT verification uses Crystal's OpenSSL HMAC and constant-time signature comparison on
  every authenticated request, with no token or response cache.
