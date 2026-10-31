# V + veb

| | |
|---|---|
| Language | V, pinned to `c36172591e766668cf8aa7cc307f05764b6a0d49` |
| Compiler backend | VC `bcfdc7f33e7b7908ee40f76d059c0e2894fb4a3b`; TCC bundle `d6e7ac1b1bcc98aed734a6ecbfa8509f24606c74` |
| HTTP server | V standard library `veb` |
| SQLite | V standard library `db.sqlite`, linked to Ubuntu's SQLite |
| **Nginx or direct** | **Direct**, listens on `HOST:PORT` |

## Running

```sh
sudo bash install.sh
bash build.sh
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 bash start.sh
```

## Design

- Routes use Veb's route attributes and request context. Veb's Linux backend uses V's epoll-based `fasthttp` server, so the application code does not parse or write HTTP frames itself.
- The pinned V install keeps `fasthttp`'s initial per-connection read/write buffers at 1/8 KiB for this API's small requests and feed payloads. The library grows either buffer when a larger request needs it.
- Database operations use `db.sqlite` calls and parameterized statements. A small channel pool gives each in-flight handler its own SQLite connection, avoiding a global request lock. The SQLite page cache is capped at 32 MiB per connection, and mmap is enabled for the seeded database.
- SQLite runs in WAL mode with `synchronous=NORMAL`. Each successful write waits for SQLite before returning its response.
- JWT and create-post JSON decode into typed structs. JWT validation uses V's `encoding.base64`, `crypto.hmac`, and `crypto.sha256` packages. It verifies HS256 and expiration for every authenticated request.
- The server listens directly, so it does not need a separate Nginx process.

## License

MIT, under the repository's [license](../../LICENSE).
