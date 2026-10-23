# Pascal + Horse

| | |
|---|---|
| Language | Free Pascal 3.2.2 (Ubuntu 24.04 package) |
| Framework | Horse 3.3.14, pinned to `84194a3683b3973f64e28835566f02468cb6e916` |
| HTTP provider | Horse's Linux `epoll` provider |
| SQLite driver | FPC `sqlite3` C bindings, backed by system SQLite |
| **Nginx or direct** | **Direct** on `0.0.0.0:80` |

## Running

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Design

- Horse is compiled with its Linux `epoll` provider, which uses one event-loop worker per CPU instead of one thread per idle client. Its idle timeout is raised to 70 seconds so upstream keep-alive sockets remain valid.
- The app serves directly, removing Nginx from the only CPU. The epoll provider handles the benchmark's idle keep-alive connections.
- Each event-loop worker owns an SQLite connection opened with `NOMUTEX`; it cannot be shared across workers. Statements are prepared once per worker and reused.
- SQLite uses WAL, `synchronous=NORMAL`, a 5-second busy timeout, memory-mapped reads and a 64 MiB page cache. Reads stay in SQLite and writes complete in SQLite autocommit mode before their responses are sent.
- The SQLite C API writes query columns directly into the response buffer, avoiding SQLDB dataset and field-object allocations. JWT HS256 verification still happens for every authenticated request, and JSON is emitted in the required key order.

## License

MIT, under the repository's [license](../../LICENSE).
