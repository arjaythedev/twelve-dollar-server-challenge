# Ruby + Rails

| | |
|---|---|
| Language | Ruby 4.0.5, built from source with YJIT |
| Framework | Rails 8.1.4 (full Rails: Active Record, Action Controller, router) |
| Server | Puma 8.0.2 |
| SQLite driver | sqlite3 gem 2.9.6 (bundled SQLite 3.53.2) |
| JWT | jwt gem 3.3.0 |
| **Nginx or direct** | **Nginx**: behind `bench/nginx.conf`, Puma on `127.0.0.1:3000` |

All gem versions, transitive ones included, are pinned in `Gemfile.lock`.

## Running it

```bash
sudo bash install.sh   # apt build deps + tzdata, compiles Ruby 4.0.5 into /opt/ruby
bash build.sh          # bundle install into vendor/bundle, bootsnap precompile
SQLITE_PATH=... JWT_SECRET=... HOST=127.0.0.1 PORT=3000 bash start.sh
```

`script/bench <VUS>` runs `bench/load.js` locally: Nginx (with `bench/nginx.conf`) and the server in one
Ubuntu 24.04 container pinned to one core with 2 GB and no swap, and k6 from the `grafana/k6` image. `DURATION` and `RAMP_UP` shorten a run.

## How it's built

- **Active Record models** `User`, `Post` and `Like` map the fixed schema. `Like` uses a composite primary
  key. Rails' own timestamps are off, so `created_at` comes from the column default and is read back with
  `RETURNING`.
- **Post reads** are one query: `Post.with_details` joins the author and selects `like_count` as a
  correlated subquery.
- **Likes** are a primary-key lookup of the post (404 if missing), then one
  `INSERT ... ON CONFLICT DO NOTHING RETURNING post_id` through `Like.insert`. A returned row means 201,
  none means 200.
- **Validation** lives on `Post`: `normalizes` trims the body, `validates` carries the spec's messages.
- **JWT** is verified on every request with the jwt gem, HS256 only.
- **Pragmas**: Rails 8's SQLite defaults (`journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`) plus
  512 MiB `mmap_size` and a 64 MiB page cache, set in `config/database.yml`.
- **Nginx in front.** Puma's reactor keeps idle keep-alive connections in an Array, so with thousands of
  clients connected directly every request pays an O(n) delete and every new connection a re-sort. Nginx
  holds the client connections and talks to Puma over at most 64 upstream connections.
- **Puma** runs in single mode with 3 threads and a 75 s keep-alive timeout.
- **YJIT** is on (`RUBY_YJIT_ENABLE=1`).
- **Log level `warn`** in production, so Rails writes no per-request lines to STDOUT.
- **Unused middleware removed** in `config/application.rb`: static files, sendfile, runtime header, method
  override, request id, remote IP, cookies, session, flash, CSP, conditional GET, ETag, tempfile reaper.
  None of them affect this API's responses, and each one costs CPU on every request.

## License

MIT, under the repo's [license](../../LICENSE).
