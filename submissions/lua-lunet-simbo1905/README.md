# lua-lunet-simbo1905 — Lua on lunet

The Feed API in plain Lua on [lunet](https://github.com/lua-lunet/lunet), a libuv + LuaJIT coroutine
runtime. The source is application logic only, in the
[realworld-lua app style](https://github.com/lua-lunet/realworld-lua/tree/main/app): `server.lua`
plus `app/` (posts, auth, health, render, config, SQLite access). Routing, JWT and HTTP/1.1 framing
all ride on the runtime's own extensions — `lunet.http_router`, `lunet.http_context`,
`lunet.security` and `lunet.http` (persist-by-default keep-alive, request-declared close honored;
[upstream #204](https://github.com/lua-lunet/lunet/issues/204)) — there is no framing shim in this
tree.

## Run behind nginx

**Behind nginx** ([`bench/nginx.conf`](../../bench/nginx.conf)). The app listens on `127.0.0.1:3000`,
loopback only; nginx terminates internet traffic on port 80.

nginx is the recommendation from the runtime's `SECURITY_ARCHITECTURE.md`: the hostile internet is
handled by hardened, battle-tested edge software and lunet is never exposed to it directly. The
edge supplies the full HTTP stack (internet-facing HTTP/1.1, TLS termination, protocol validation,
limits, buffering) and the app answers origin-form requests on loopback. The keep-alive contract is
honored in-process: the server loop reads requests and writes responses on the same connection for
as long as the client keeps it open, holds nginx's idle upstream connections past the 65 s the SPEC
requires, and sends no `Connection: close` unless the socket is actually closing. nginx holds the
internet-facing keep-alive connections on :80 and reuses its cached upstream connections
(`keepalive 64`). Direct mode also works per the SPEC: every k6 user holds its own keep-alive
connection (up to ~15,000 open connections), for which `start.sh` mirrors the SPEC's fd grant. The
k6 baseline (50 VUs) measured p99 under 1 ms with 0% errors against this path.

## Runtime

lunet is not compiled from source. `build.sh` downloads the pinned release archive
(`lunet-linux-amd64.tar.gz`, tag `v0.14.1`), verifies its sha256 digest against a hash embedded in the
script, stages it into `.lunet/`, and smoke-tests the loader — the same way a Node submission
installs Node. No xmake, no cargo. Nothing prebuilt is committed.

## Build and run

```bash
sudo bash install.sh   # once, as root: apt libraries, re-run safe
bash build.sh          # fetch + verify + stage the pinned runtime (.lunet/)
bash start.sh          # foreground server, configured only by the SPEC.md env vars
```

Local helper: `bin/dev.sh start|stop|status` — pid-file managed, fresh copy of the seed database,
log in `target/server.log`.

## Requirements

Ubuntu 24.04 x86_64 and nothing to bootstrap: no compiler, no language toolchain. `install.sh` needs
curl plus apt packages only (`ca-certificates`, `libluajit-5.1-2`, `libuv1`, `zlib1g`,
`libsqlite3-0`, `libsodium23`) and symlinks `libsodium.so` so the FFI can load it.

## Design notes

- **SQLite in-process** at `$SQLITE_PATH` via lunet's bundled SQLite driver; the provisioned file is
  already WAL, and `synchronous=NORMAL` is passed through the driver's open contract so writes are
  durable before a 201 is sent. The driver's `open` returns a
  pair: writes (and the insert) run on the read-write handle through the worker pool; reads run on
  the read-only handle inline on the loop thread — uncontended against the single writer in WAL
  (`SELECT`s on the read-only handle fail loudly with `SQLITE_READONLY` if a write is ever
  mis-routed there). No other service, no cache tiers.
- **No cross-request caching.** Every request runs its SQL against SQLite. Every JWT is verified
  fresh per request through `lunet.security`: the signature is recomputed and compared in constant
  time (libsodium via `lunet.sodium`), header and payload decoded and checked, `exp` enforced.
- **Bytes-exact JSON.** Responses are rendered directly in the byte shapes the spec mandates
  (five fixed shapes assembled with one escaper sourced from jsonic's vendored dkjson), so the
  key order and bytes match by construction rather than by reordering a generic encode.
- **fd grant mirrored, not tuning.** `start.sh` raises its own process's fd soft limit to the 65535
  the scoring harness grants the service (`LimitNOFILE=65535`); rule 11 makes in-process settings
  fair game. Without it a plain-shell start hits the 1024 default and starves keep-alive
  connections under load.

## Benchmark

On the scoring box shape — 1 vCPU / 2 GiB, no swap, nothing tuned (aarch64 lima VM standing in for
the droplet) — holding 2,500 VUs for the full 5 minutes with `bench/load.js`, with the load
generator itself sharing the same 1 vCPU (strictly harder than the scored setup, where k6 runs from
a separate machine):

- **checks 100.00% — 93,477 requests, 0 failed** (feed 200, post 200, like 200/201, create 201);
  **p95 0.45 ms, p99 0.79 ms**; server peak RSS 16.9 MB for the whole run (lunet `v0.14.1`).
