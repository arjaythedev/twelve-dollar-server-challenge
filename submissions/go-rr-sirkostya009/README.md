# Go + rr

|                     |                                                                                                                             |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| Language            | Go 1.27.2 (official tarball)                                                                                                |
| Router              | [rr](https://github.com/sirkostya009/rr) `a53db0a`, codegen: one generated `ServeHTTP`, no runtime router                   |
| JSON                | [ggen](https://github.com/sirkostya009/ggen) `38ae500`, codegen encoders/decoders with decode-time validation               |
| HTTP server         | `net/http`                                                                                                                  |
| SQLite driver       | [crawshaw.io/sqlite](https://github.com/crawshaw/sqlite) `d196488`, cgo with its bundled SQLite 3.38.5 compiled from source |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (not yet confirmed with a 1-CPU k6 run)                                               |

All Go dependencies are pinned in `go.mod`/`go.sum`. The generated router (`internal/httpapi/api_gen.go`) and
codecs (`*_ggen.go`) are committed, so `build.sh` only compiles; `GOEXPERIMENT=simd go generate ./...` rebuilds them.

## Running it

```bash
sudo bash install.sh   # apt: build-essential; Go 1.27.2 into /usr/local/go
bash build.sh          # bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Layout

The layers of the goserver project it is adapted from:
`httpapi.Server` (auth) → rr-generated router → controller → service → repository → crawshaw.io/sqlite.

- `domain/`: wire types with `json:` and ggen `pipe:` tags, and the error sentinels, whose text is the spec's message.
- `internal/httpapi/`: rr routes, the `requireUser` guard, and the one place errors map to statuses. Unexpected
  errors are logged with `context.WithoutCancel`, so a client hanging up doesn't cost the log line.
- `internal/service/`, `internal/repository/`: the business and data layers. The repository returns errors unwrapped
  (one query per method, so the origin is clear) and `sql.ErrNoRows` when an `INSERT ... RETURNING` yields no row.
- `internal/auth/`: HS256 JWT verification.

## Optimizations, and why

- **Codegen routing and JSON.** rr compiles the five routes into a switch, and ggen encodes responses into
  pooled buffers with no reflection. Output keys keep declaration order (`nosortkeys`) to match the spec. The router
  is generated with `rr -nopathvalue`: handlers get the id as an argument, so `r.SetPathValue` would be dead work.
- **AVX2 JSON codecs.** The `domain` codecs are generated with `ggen -simd avx2` and built with `GOEXPERIMENT=simd`:
  string and escape scans run 32 bytes at a time, about 2x faster on feed encoding and post decoding. AVX2 is on
  every Basic droplet host, AVX-512 isn't. The JWT codecs stay scalar (`-simd off`): token parts are too short
  for vectors to pay off.
- **Validation at decode time.** `CreatePost.Body` is `required trim notempty maxrunes=500`. A non-string `body`
  (number, bool, null, object, array) fails at its first byte with `ggen.ErrExpectString` at path `body`, which maps
  to `body is required`. Any other parse error, an unterminated or badly escaped string included, is
  `malformed JSON body`. The body is read once.
- **Auth before body and id.** rr runs `//rr:pre` guards before it decodes the body or the handler parses the id.
  Requests without an `Authorization` header skip token work entirely.
- **JWT by hand**: HS256 only, `hmac.Equal`, `exp` checked. Every token is verified and nothing is cached (rule 5).
  Keyed HMAC states are pooled together with the array the sum goes into, and the signed bytes reach `Write` through
  `unsafe.Slice` instead of a `[]byte` copy. Both would otherwise escape through the `hash.Hash` interface: 4
  allocations per verify instead of 6.
- **SQLite**: crawshaw.io/sqlite, no `database/sql`. One connection behind a mutex, in `locking_mode=EXCLUSIVE`:
  on one core a reader pool never runs in parallel, and exclusive mode keeps the WAL index in heap memory instead
  of the `-shm` file, so a read takes no file locks (about 40% more throughput than 4 shared-mode readers in a 1-core
  run). The connection keeps its prepared statements, prepared once at startup. WAL, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 32 MiB page cache. A like is
  one statement (`INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`); a post-existence check runs only
  when it inserted nothing, to tell a repeat from a missing post.
- **Runtime pinned in code** for the 1 vCPU, 2 GB box: `GOMAXPROCS=1`, GC off until a 768 MiB `GOMEMLIMIT`
  (the live heap is tiny, so GC rarely takes the only core).
- **Keep-alive**: 120 s idle timeout, above Nginx's 65 s.

## Known gaps

None of these are covered by the test suite.

- A top-level JSON value that is not an object (`[]`, `null`) answers `malformed JSON body`, not `body is required`.
- A broken non-string `body` (`{"body":tru}`) answers `body is required`, not `malformed JSON body`: ggen stops at the
  value's first byte, so it can't tell it from a valid number or bool.
- Trailing bytes after the object (`{"body":"ok"}x`) are ignored, and the post is created.

## License

MIT, under the repo's [license](../../LICENSE).
