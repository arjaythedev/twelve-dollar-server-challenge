# javascript-bun-zxmpg41

- **Language/runtime:** JavaScript on Bun 1.4.3 (pinned in `install.sh`)
- **HTTP:** `Bun.serve` with hand-written routing, no framework
- **SQLite driver:** `bun:sqlite` (built in)
- **JWT:** hand-rolled HS256 using `node:crypto` HMAC and a constant-time compare. Every token is verified on every request.
- **Dependencies:** none
- **Nginx or direct:** **direct**. The server listens on `HOST:PORT` (`0.0.0.0:80`) and Bun's uWebSockets-based server holds the ~15k keep-alive connections itself. `idleTimeout` is 255 s.

## Optimizations

- **One process, one connection.** There is 1 vCPU, so more processes or threads would only add context switches and duplicate page caches.
- **JSON is built inside SQLite.** `json_object`/`json_group_array` produce the exact response body in key order, and the server sends that string as is. No row objects are created and nothing is serialized in JS on the read path.
- **Group commit (rule 6).** Write requests are queued and flushed once per event-loop turn (`setImmediate`) inside a single `BEGIN IMMEDIATE … COMMIT`. Each response is sent only after its commit has finished.
- **Pragmas.**
  - WAL with `synchronous=NORMAL`.
  - 64 MB `cache_size`, 512 MB `mmap_size`, `temp_store=MEMORY`.
- **Prepared statements, reused.** Each statement is prepared once at startup and reused, as rule 5 allows.

## Local "$12 server" simulation (Docker)

```bash
bash seed/make-seed.sh                                     # once, from the repo root
bash submissions/javascript-bun-zxmpg41/docker/test.sh     # build, start on a fresh DB, run the 42 checks
VUS=20000 RAMP_UP=120s bash submissions/javascript-bun-zxmpg41/docker/bench.sh   # k6 in a container
bash submissions/javascript-bun-zxmpg41/docker/down.sh
```

How the container matches the droplet:

- Runs `ubuntu:24.04`.
- Runs `install.sh` as root, then `build.sh` as a non-root user.
- Limited to 1 CPU and 2 GB RAM, with no swap and `nofile` set to 65535.
- Gets a fresh copy of `seed/feed.db` on every start.
- Serves on container port 80, which is published on host port 8080.

On Apple Silicon, `install.sh` installs the arm64 Bun build so Docker doesn't have to emulate x86.

**Colima:** start the VM with its own IP address (`colima start --cpu 4 --memory 6 --network-address`). 

`bench.sh` runs k6 as an opt-in compose service (profile `bench`) on the Docker network. The server image is unchanged. Traffic stays inside the VM, so the macOS bridge and the Mac's 16k outgoing-port limit don't apply. k6 is limited to `K6_CPUS` (default 2), so the VM needs at least 4 CPUs.
