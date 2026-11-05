# Node + Fastify

Tokens are verified in `auth.ts` with `node:crypto`: one HMAC per request,
constant-time signature compare, `exp`/`nbf` checked every time. Everything
else is tuned around that core — SQLite BLOB bodies, pragmas, keep-alive
tuning.

|                     |                                                                                    |
| ------------------- | ---------------------------------------------------------------------------------- |
| Language            | TypeScript 7.0.2, compiled to ESM for Node.js 24.21.0 (official linux-x64 tarball) |
| Framework           | Fastify 5.12.7                                                                     |
| SQLite driver       | better-sqlite3 13.0.3 (synchronous, compiled against Ubuntu's SQLite)              |
| Types               | `@types/node` 24.19.2, `@types/better-sqlite3` 9.6.0 (dev only)                    |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself                                             |

All dependency versions are pinned in `package.json` + `package-lock.json`.

## Layout

```
src/
  server.ts    # entrypoint: builds the app and listens on HOST:PORT
  app.ts       # Fastify instance, parsers, error handlers, all five routes
  db.ts        # the single SQLite connection, pragmas, prepared statements
             # (reads/creates return whole response bodies as Buffers)
  auth.ts      # HS256 verification (header fast path, one HMAC per request,
             # constant-time compare; every request, nothing cached)
  ids.ts       # post-id validation
  config.ts    # SQLITE_PATH, JWT_SECRET, HOST, PORT from the environment
  responses.ts # compact-JSON helpers for errors, health and likes
             # (post/feed bodies come straight from SQLite, no JS serialization)
```

`tsconfig.json` uses `strict` plus `noUncheckedIndexedAccess`,
`exactOptionalPropertyTypes`, `noPropertyAccessFromIndexSignature`,
`useUnknownInCatchVariables` and `noEmitOnError`, so `build.sh` fails rather
than shipping a type error. One deliberate addition to the base options is
`esModuleInterop` (emit-neutral for ESM output): Fastify's and
`@types/better-sqlite3`'s typings use `export =`, which TypeScript cannot
default-import under `module: ESNext` without it.

## Running it

```bash
sudo bash install.sh   # Node 24.21.0 + build-essential, python3, curl
bash build.sh          # npm ci, then tsc -p tsconfig.json (src/ -> dist/)
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **One process, sync SQLite calls inline on the event loop.** The box has one vCPU, so worker processes or a threadpool would only add context switches. Every query is an index or primary-key lookup finishing in well under a millisecond. One connection also means writes never wait on SQLite locks, so no `SQLITE_BUSY` retries.
- **Direct instead of Nginx.** On one core, Nginx's CPU comes straight out of Node's budget, and p99 < 1 s is the limit that fails first near saturation. Node's HTTP server holds ~15k idle keep-alive connections cheaply. `keepAliveTimeout` is 75 s (SPEC wants >= 65 s behind Nginx; direct clients also reuse long-lived keep-alives) with `headersTimeout` 80 s set on the underlying HTTP server (Fastify v5 exposes no such option) and `backlog` 4096 on `listen`, as Node requires `headersTimeout > keepAliveTimeout`.
- **Lean Fastify setup.** No schema validation, no logger, no global auth hook (GETs pay zero JWT cost). A single raw-buffer JSON content parser feeds `POST /posts` (plus a `'*'` fallback so exotic content types map to the SPEC's `malformed JSON body` instead of Fastify's 415), which parses with `JSON.parse` once. Like responses are template-built `Buffer`s (`post_id` is a validated integer, so interpolation is exact); errors stay `JSON.stringify`'d small literals on cold paths.
- **Whole response bodies come from SQLite as BLOBs.** The feed is one `group_concat` of per-row `json_object(...)` fragments with the `{"posts":[ ... ]}` envelope concatenated in SQL; single posts and `INSERT ... RETURNING` likewise return the full `{"post":...}` body. Statements are `.pluck()`ed, so better-sqlite3 hands handlers a `Buffer` that Fastify sends without decoding to string or re-encoding — no per-row object allocation, no JS string building, no serializer on the hot paths. Key order follows the `json_object()` argument order and escaping matches `JSON.stringify` byte-for-byte (verified against the golden files). No response caching anywhere: every request reads SQLite while being served (rule 5).
- **HS256 JWT verification.** One `crypto.createHmac` per request, constant-time signature compare, `exp`/`nbf` checked — every token verified on every request (rule 5: no remembered verifications). A prefix fast path skips the header parse for the standard `{"alg":"HS256","typ":"JWT"}` header, which is what all seed and test tokens use; anything else goes through a full HS256-only header check. `auth.ts` preserves the SPEC's exact error split (`missing bearer token` / `invalid or expired token` / `invalid token payload`, auth before id).
- **SQL**: the reference queries. Each like is a single statement, `INSERT ... SELECT ... WHERE EXISTS (post) ON CONFLICT (user_id, post_id) DO NOTHING` (anonymous placeholders bound positionally — better-sqlite3 counts each `?NNN` occurrence separately, so `?1`/`?2` numbering throws); a post-existence check runs only when it inserted nothing, to tell "already liked" from "no such post". Prepared statements are reused (plan cache, not data cache).
- **Pragmas**: `locking_mode=EXCLUSIVE` (single process: the file lock is taken once, not per statement), `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), `foreign_keys=OFF` (better-sqlite3 enforces FKs by default — verified on a fresh db — so this skips a PK probe per insert and matches the reference implementations), 1 GiB `mmap_size`, 64 MiB page cache, `temp_store=MEMORY`, `journal_size_limit` 64 MiB, `wal_autocheckpoint` 10k pages (fewer event-loop-blocking fsync batches under writes; durability is unaffected). No `busy_timeout`: with one connection in exclusive mode it can never fire. Writes run in autocommit, so each 201 is committed before the response.
- **Body validation** uses native `String.prototype.trim()` (exactly the SPEC's trim set) and codepoint length (`[...s].length`) for the 500-character limit.

## License

MIT, under the repo's [license](../../LICENSE).
