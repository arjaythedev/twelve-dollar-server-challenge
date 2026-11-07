# julia-raw-amanica

Julia 1.13.3 · no framework · SQLite 3.54.1 (amalgamation compiled from source, called with `ccall`) · **direct** (no Nginx)

Work in progress. See [PLAN.md](PLAN.md) for the roadmap and the decisions log.

## Current state

- Single-threaded, level-triggered epoll loop on raw file descriptors (no libuv, no Tasks).
- HTTP/1.1 parsed in place in the connection's read buffer; responses written straight into reusable
  byte buffers (no intermediate objects, no JSON library).
- JWT HS256 verified on every request with libcrypto's `HMAC`.
- Prepared statements via the SQLite C API. Reads go through a read-backend interface (`READ_BACKEND`
  in `config.sh`; only `sql`, the public-API backend, exists so far).

## Files

| File | |
|---|---|
| `config.sh` | pinned versions (Julia, SQLite) and build switches |
| `install.sh` | root: build-essential, curl, unzip, openssl; the official Julia tarball (sha256 checked) |
| `build.sh` | user: downloads the SQLite amalgamation (SHA3-256 checked), compiles `build/libjr.so` |
| `start.sh` | runs `src/main.jl` |
| `src/` | the server |
| `dev/` | local testing in podman (not used on the box): `dev/ct.sh test`, `dev/ct.sh stress 5000 2m` |
