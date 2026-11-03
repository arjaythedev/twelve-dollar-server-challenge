# Haskell + Warp

|                     |                                                                                           |
| ------------------- | ----------------------------------------------------------------------------------------- |
| Language            | Haskell, GHC 9.10.3 (installed with ghcup 0.2.6.2)                                        |
| Build               | cabal-install 3.16.1.0, Hackage index pinned to `2026-10-15T11:34:24Z` in `cabal.project` |
| Server              | Warp 3.4.16 / WAI 3.2.5                                                                   |
| SQLite driver       | direct-sqlite 2.3.29 (bundles SQLite 3.45.0)                                              |
| JSON                | **none**, SQLite does it all (see below)                                                  |
| JWT                 | hand-rolled: cryptohash-sha256 0.11.102.1 (HMAC) + base64-bytestring 1.2.1.0              |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself                                                    |

## Running it

```bash
sudo bash install.sh   # ghcup -> GHC + cabal into /opt/.ghcup, linked into /usr/local/bin
bash build.sh          # cabal build, binary copied to bin/server
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

Ubuntu's own `cabal-install` (3.8) can't verify Hackage's current root keys, so `cabal update` fails with it.
That's why `install.sh` uses ghcup instead of apt.

- **Smaller per-connection buffers.** Warp gives each connection a 16 KB send and a 16 KB receive buffer.
  Requests here are under 1 KB and the biggest response (the feed) is ~5 KB, so I build connections myself
  via `Network.Wai.Handler.Warp.Internal` with an 8 KB write buffer and a 4 KB receive buffer.
- **`-kc4k`.** GHC grows thread stacks in 32 KB chunks by default, and Warp's request path outgrows the
  initial 1 KB stack, so every connection was paying for a 32 KB chunk. 4 KB chunks saved ~100 MB at 15k
  connections.
- **`-A4m -I0`.** Smaller allocation area (less resident memory), and no idle-time GC.
- **`mmap_size=0`.** Otherwise the whole 238 MB database gets mapped into the process. The hot data (the
  newest posts and their likes) fits in a 32 MB page cache.
- **`stepNoCB`** instead of `step`: an unsafe FFI call, so no OS-thread handoff per row. These queries
  take well under a millisecond and I register no SQLite callbacks.
- **One read connection.** With one core, more connections only add switching.

## Numbers

Measured on my machine, not the real droplet: Docker on Apple silicon, server container limited to
1 CPU / 2 GB / no swap, k6 in a separate container, `bench/load.js`.

| Users                 | Errors | p95     | p99     | Server CPU | Server memory |
| --------------------- | ------ | ------- | ------- | ---------- | ------------- |
| 2,500                 | 0%     | 1.16 ms | 2.32 ms |            |               |
| 15,000 (~1,250 req/s) | 0%     | 0.78 ms | 2.65 ms | ~26% peak  | ~370 MB RSS   |

Memory went from ~37 KB to ~24 KB per connection with the buffer and stack changes. On a real DigitalOcean
vCPU I'd expect CPU to become the limit well before memory does.

## License

MIT, under the repo's [license](../../LICENSE).
