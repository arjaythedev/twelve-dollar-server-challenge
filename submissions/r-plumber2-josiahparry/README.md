# R 

This is a pure R implementation of the $12 server challenge.
It is written using `{plumber2}`, `{DBI}`, `RSQLite`, `{yyjsonr}`, and `{jose}`.

- Note that we use `yyjsonr` instead of the default jsonlite serializer and parser for speed and memory efficiency.
- We actively don't use plumber2's parameter typing as that is the only way to be compatible with these tests.
  - I would recommend `<id:integer>` to those making a production plumber2 api
- Auth is handled using `jose` for JWT decoding


| | |
|---|---|
| Language | R 4.6.1 |
| Framework | plumber2 0.2.0 |
| SQLite driver | RSQLite 3.53.3 via DBI 1.3.0 |
| JSON | yyjsonr 0.1.22 |
| JWT | jose 2.0.0 |
| **Nginx or direct** | **Behind Nginx** on `127.0.0.1:3000` |

Packages are installed from the 2026-10-07 Posit Package Manager snapshot.


## Running

```bash
sudo bash install.sh
bash build.sh
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

## Optimizations

- yyjsonr serializes responses and parses request bodies
- RSQLite connects with `bigint = "integer"`.
- Likes use a single `INSERT ... WHERE EXISTS ... ON CONFLICT DO NOTHING RETURNING post_id`.
- SQLite pragmas match the Python submission.
- Request logging is off.

## License

MIT, under the repo's [license](../../LICENSE).
