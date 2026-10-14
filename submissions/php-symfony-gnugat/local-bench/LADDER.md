# The ladder

How many users the app holds, one optimisation at a time, in both FrankenPHP modes: classic (Symfony boots on every
request, like PHP-FPM) and worker (Symfony boots once per worker). One commit per phase, each with its table.

**Setup**: `local-bench/bench.sh` runs the server in an Ubuntu 24.04 container pinned to one core of an Apple silicon
Mac (OrbStack), with 2 GB and no swap; k6 1.4.0 runs `bench/load.js` in its own containers on the other 14 cores.
Like the real score: a 1,000-user 2 min warm-up (not measured), 60 s ramp-up, 5 min hold, 30 s ramp-down; a run
passes with p95 < 0.5 s, p99 < 1 s and < 1% errors. `find-limit.sh` steps up, then bisects to 250 users: each table
shows the last passing run and the first failing one. Hold CPU is the server's median CPU during the hold.
Measured 2026-10-16 to 2026-10-19; phase 0 measured at the start and the end gives the drift: one step (250 users).

**Only compare these numbers with each other**: one core here is much faster than the droplet's vCPU, and any
droplet estimate made from them is rough.
