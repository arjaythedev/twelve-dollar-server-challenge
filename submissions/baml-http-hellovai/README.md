# Packed BAML / native HTTP / SQLite

This submission measures a **packed BAML executable**, produced by `baml pack main`.
`start.sh` executes `bin/server` directly. Routing, SQL selection, validation, HS256
JWT verification, and JSON responses are implemented in `server.baml`.

## Stack and build

- BAML source: BoundaryML/baml commit `99edb5d249850ed92db987148feb78f551a93250`
  (development runtime, not a published BAML release).
- Rust toolchain: `1.98.0`.
- HTTP: BAML's native `baml.http.Server` (Hyper/Tokio); dependencies pinned by
  the upstream Cargo.lock.
- SQLite: `rusqlite 0.37.0`, linked to Ubuntu 24.04's system SQLite.
- `sqlite.patch` adds a minimal `baml.sqlite.query(path, sql, params_json)` API
  to that pinned runtime, including the native provider and lockfile change.
  The patch is included here so CI and the benchmark can build it from source
  without needing a separately merged BAML change or a local checkout.

**Run behind Nginx**, listening on `HOST:PORT` (normally `127.0.0.1:3000`).
HTTP/1.1 keep-alive is enabled, with a 75-second header timeout. The server uses
one Tokio worker thread for the single-vCPU box.

```bash
sudo bash submissions/baml-http-hellovai/install.sh
bash test/run.sh submissions/baml-http-hellovai
```

`build.sh` fetches the pinned source, applies the patch, builds both `baml-cli`
and `baml-pack-host` in release mode, and packs the application using that local
host. The native SQLite binding is therefore present in the packed binary.
Compilation is serial with LTO disabled and 16 codegen units to reduce build
memory on the 2 GB VM; runtime optimization remains `opt-level=3`.
Build artifacts stay in ignored `.build/` and `bin/` directories.

## SQLite API and request behavior

The provider reuses one in-process SQLite connection per database path, serialized
by a mutex, and caches up to 32 prepared statements. It binds parameters rather
than interpolating request data into SQL. Its input is a JSON array of null,
numbers, or strings; its output is `{"rows":[[...]],"changes":<int>}`. Blob
results are intentionally unsupported by this small API.

Every request queries SQLite anew. There is no response, data, or authentication
cache. The connection uses WAL, `synchronous=NORMAL`, a 5-second busy timeout,
foreign-key enforcement, and a 64 MiB SQLite page cache. Each statement runs in
autocommit and is stepped to completion, including `INSERT ... RETURNING`, before
the API returns and the HTTP response is sent.

Feed reads use the fixed schema's index ordering and correlated like counts.
Like writes use `INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`
so a missing post is distinguished from a duplicate without relying on a cached
post list. HMAC-SHA256 is computed in BAML using the native SHA-256 primitive;
every request checks the signature, algorithm, expiry, and token payload.

## Validation

The repository's 42 correctness checks pass against the packed executable on a
fresh seed database. The native provider also has a Rust regression test for
parameter binding, externally visible commits after `RETURNING`, fresh reads after
another connection updates a row, and invalid parameter input:

```bash
# From the patched source's baml_language directory:
cargo test --locked -p sys_native --lib sqlite::tests --features bundle-http
```

A local correctness run does not establish a benchmark score. Scoring should use
this release-packed executable with the repository's normal k6 procedure on the
specified VM.
