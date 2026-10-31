# Local load rig

`loadtest.sh` runs the server and k6 **inside Docker Desktop's VM** on a private
network, with the server capped at the challenge box:

| Resource | Limit |
|---|---|
| CPU | 1.0 (`--cpus=1`) |
| Memory | 2 GiB, no swap |
| Open files | 65,535 |
| DB | fresh copy of `seed/feed.db` per run |

The generator is not capped, so the server runs on one vCPU while k6 uses the rest.

## Why Docker for both ends

On this WSL2 host, driving more than roughly 6k concurrent loopback connections
makes `connect()` stall for 30 s (`dial: i/o timeout`) regardless of k6 or the
server. A plain Go dial flood reproduces it; the kernel SYN-drop counters stay at
zero. Running both ends on the Docker VM's network removes that ceiling, so the
server can be exercised at 10k+ VUs without generator-side noise.

This is a local development aid, not part of the submission's build or CI.

## Usage

```bash
K6_BIN=/path/to/k6 dev/loadtest.sh 10000 60s 5m
```

`K6_BIN` must be a static linux/amd64 k6 binary. Optional `SCRIPT` overrides the
k6 script (default `bench/load.js`); `dev/conn-test.js` is a connection-only probe.

The script refuses a run unless the responding server has `uptime_s <= 5` (so a
stale listener cannot answer), then prints request rate, p95/p99, connect p95,
failed-request rate, classified k6 errors, server cgroup throttling, and peak
memory.

## Caveats

- Docker Desktop's VM shares the host CPU; this is an emulation of the 1 vCPU /
  2 GB droplet, not the official benchmark.
- The official score comes from the challenge's separate 16-core load generators.
