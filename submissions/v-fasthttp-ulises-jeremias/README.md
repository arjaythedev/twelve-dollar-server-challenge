# V + fasthttp

| | |
|---|---|
| Language | V, pinned to `d67ee5500338554f54c7c8ca4cc00df42159577d` |
| HTTP server | V standard library `fasthttp` (Linux epoll) |
| SQLite | Prepared SQLite C API statements, linked against Ubuntu's SQLite |
| **Nginx or direct** | **Direct**, listens on `HOST:PORT` |

## Running

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- A single epoll worker matches the one-vCPU benchmark VM. `fasthttp` reuses each connection's request/response buffers and performs no framework middleware dispatch.
- One SQLite connection is opened per worker with prepared statements for feed, post lookup, health, post creation, like creation and existence checks. Statements are reused without a lock. WAL and `synchronous=NORMAL` provide durable commits before write responses. Every request obtains current values from SQLite.
- The pinned V install sets fasthttp's initial per-connection read/write buffers to 1/8 KiB. The API's requests fit those capacities, and fasthttp grows them for larger payloads.
- HS256 signatures and expiration are checked on every authenticated request. No request or token result is cached.
- Direct serving avoids spending the single core on Nginx. The server's idle timeout is set beyond the required 65 seconds for upstream keep-alive reuse.
- Responses on descriptors 60,000 and above close that keep-alive connection after the response. This leaves room below the challenge's 65,535 per-process descriptor limit; clients reconnect on a later request, and no in-flight write is dropped.

## License

MIT, under the repository's [license](../../LICENSE).
