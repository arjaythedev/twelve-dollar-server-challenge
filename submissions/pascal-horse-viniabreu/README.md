# Pascal + Horse

| | |
|---|---|
| Language | Free Pascal 3.2.2 (Ubuntu 24.04 package) |
| Framework | Horse 3.3.14, pinned to `84194a3683b3973f64e28835566f02468cb6e916` |
| HTTP provider | Horse's Linux `epoll` provider |
| SQLite driver | FPC `sqlite3conn` / `sqldb`, backed by system SQLite |
| **Nginx or direct** | **Nginx** on `127.0.0.1:3000` |

## Running

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Design

- Horse is compiled with its Linux `epoll` provider, avoiding a thread per idle client.
- Each serving thread owns its SQLite connection and transaction objects; connections are not shared across threads.
- SQLite uses WAL, `synchronous=NORMAL`, a busy timeout, memory-mapped reads and a 64 MiB page cache. Every endpoint reads live data from SQLite; no response, table or JWT-verification cache is used.
- JWT HS256 verification is implemented locally and happens for every authenticated request. JSON responses are written directly to preserve the exact key order required by the challenge.

## License

MIT, under the repository's [license](../../LICENSE).
