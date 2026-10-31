# Local load rig

`loadtest.sh` runs the server and k6 **inside Docker Desktop's VM** on a private
network. The server gets the challenge box's resources; the generator is capped
so a run cannot take the whole host down.

| Process | CPU | Memory | Other |
|---|---|---|---|
| `twelve-server` | CPU 0, 1.0 CPU quota | 2 GiB, no swap | 65,535 fds |
| `twelve-k6-*` | CPUs `1..nproc-3` | 3 GiB each | `K6_PROCS` containers |
| WSL / Docker daemon | last two CPUs | — | reserved |

On a 12-vCPU host that means server on CPU 0, k6 on 1–9, and CPUs 10–11 left for
WSL/Docker. A cgroup quota alone does not guarantee a core: without pinning, k6
can saturate all 12 cores and the server's 1-vCPU share gets time-sliced, which
both invalidates the result and can make the host unresponsive. That is exactly
what happened with a 25k-VU single-container run.

## Port ceiling

One k6 container has only the VM's ephemeral port range (~28k ports), and every
VU holds one keep-alive connection. Above ~20k VUs in a single container k6 runs
out of source ports, starts reconnecting, and the retry storm can swamp both the
server and the host. The script refuses `VUS > 20000` with `K6_PROCS=1`; use
multiple containers above that, each with its own source IP.

| Layout | Practical VUs |
|---|---:|
| `K6_PROCS=1` | ≤ 20,000 |
| `K6_PROCS=2` | ≤ 40,000 |
| `K6_PROCS=3` | ≤ 60,000 (generator CPU becomes the limit) |

The rig's generator has `nproc-2` cores, so in practice 25k–40k is the useful
range; beyond that k6 itself becomes the bottleneck before the server does.

## Watchdog

`LOAD_MAX` (default `nproc+2`) is sampled every 2s. If host load exceeds it, the
script kills the k6 containers and stops, leaving the server and DB intact. Set
`LOAD_MAX=0` to disable, or lower it for an even quieter host.

## Usage

```bash
K6_BIN=/path/to/k6 dev/loadtest.sh 10000 60s 5m
K6_BIN=/path/to/k6 K6_PROCS=2 dev/loadtest.sh 25000 60s 2m
```

`K6_BIN` must be a static linux/amd64 k6 binary. Optional `SCRIPT` overrides the
k6 script (default `bench/load.js`); `dev/conn-test.js` is a connection-only probe.
The script refuses a run unless the responding server has `uptime_s <= 5`, so a
stale listener cannot answer.

Overrides: `SERVER_CPUSET`, `K6_CPUSET`, `K6_MEMORY`, `LOAD_MAX`, `K6_PROCS`.

## Caveats

- Docker Desktop's VM shares the host; this emulates 1 vCPU / 2 GB, it is not the
  official benchmark.
- The official score comes from the challenge's separate 16-core load generators,
  where no generator/host contention exists at all.
- This directory is a development aid, not part of the submission build or CI.
