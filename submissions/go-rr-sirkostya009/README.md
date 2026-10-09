# Go + rr

| | |
|---|---|
| Language | Go 1.27.2 (official tarball) |
| Router | [rr](https://github.com/sirkostya009/rr) `a53db0a`, codegen: one generated `ServeHTTP`, no runtime router |
| JSON | [ggen](https://github.com/sirkostya009/ggen) `38ae500`, codegen encoders/decoders with decode-time validation |
| HTTP server | `net/http` |
| SQLite driver | a small cgo binding (`internal/sqlite`) to Ubuntu's libsqlite3 3.45.1 (`libsqlite3-dev`) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (not yet confirmed with a 1-CPU k6 run) |

All Go dependencies are pinned in `go.mod`/`go.sum`. The generated router (`internal/httpapi/api_gen.go`) and
codecs (`*_ggen.go`) are committed, so `build.sh` only compiles; `go generate ./...` rebuilds them.

## Running it

```bash
sudo bash install.sh   # apt: build-essential, libsqlite3-dev; Go 1.27.2 into /usr/local/go
bash build.sh          # bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Layout

The layers of the goserver project it is adapted from:
`httpapi.Server` (auth) → rr-generated router → controller → service → repository → SQLite.

- `domain/`: wire types with `json:` and ggen `pipe:` tags, and the error sentinels, whose text is the spec's message.
- `internal/httpapi/`: rr routes, the `requireUser` guard, and the one place errors map to statuses.
- `internal/service/`, `internal/repository/`: the business and data layers.
- `internal/auth/`: HS256 JWT verification.
- `internal/sqlite/`: the cgo binding.

## Optimizations, and why

- **Codegen routing and JSON.** rr compiles the five routes into a switch, and ggen encodes responses into
  pooled buffers with no reflection. Output keys keep declaration order (`nosortkeys`) to match the spec.
- **Validation at decode time.** `CreatePost.Body` is `required trim notempty maxrunes=500`. Its non-string JSON
  shapes (number, bool, object, null) are accepted by converters that fail with `ErrBodyNotString`, so a wrong-type
  body and malformed JSON come back as different errors without reading the body twice.
- **Auth before body and id.** rr runs `//rr:pre` guards before it decodes the body or the handler parses the id.
  Requests without an `Authorization` header skip token work entirely.
- **JWT by hand**: HS256 only, `hmac.Equal`, `exp` checked, keyed HMAC states pooled. Every token is verified;
  nothing is cached (rule 5).
- **SQLite**: no `database/sql`. A pool of read-only connections and one writer connection behind a mutex, each with
  its statements prepared once. WAL, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 32 MiB page cache. A like is
  one statement (`INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`); a post-existence check runs only
  when it inserted nothing, to tell a repeat from a missing post.
- **Keep-alive**: 120 s idle timeout, above Nginx's 65 s.

## Known gaps

- A `body` holding an array (`{"body":[1]}`) and a top-level JSON value that is not an object (`[]`, `null`) answer
  `malformed JSON body`, not `body is required`: ggen converters can't take container inputs. The test suite
  doesn't cover either.

## License

MIT, under the repo's [license](../../LICENSE).
