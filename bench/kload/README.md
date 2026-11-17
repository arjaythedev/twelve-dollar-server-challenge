# kload: the $12-server-challenge load test, in Kotlin

> Standalone repo: https://github.com/3x3xX3N0N/kload (MIT, same as this repo). Here: `cd bench/kload && gradle build && ./kload --vus 1000`.
> It needs a JDK 21+ and Gradle; `seed/tokens.json` is found automatically.

A drop-in replacement for `bench/load.js` + k6 in [twelve-dollar-server-challenge](https://github.com/arjaythedev/twelve-dollar-server-challenge).
It simulates the same users, with the same schedule and the same pass/fail thresholds, and uses far less memory per user.
It's written in plain Kotlin on the JVM with no dependencies: a few NIO event-loop threads, and each virtual user is a
small state machine rather than its own goroutine with its own JS runtime.

```bash
gradle build
./kload --vus 64000 --url http://SERVER:80 --tokens ../twelve-dollar-server-challenge/seed/tokens.json
./kload --help
```

It honours k6's env vars too (`BASE_URL`, `VUS`, `DURATION`, `RAMP_UP`, `TOKENS`). Exit code 0 = pass, 99 = thresholds failed (like k6).

## Same behaviour as `bench/load.js`

| `load.js` | kload |
|---|---|
| `ramping-vus`: 0 → N over `RAMP_UP`, hold `DURATION`, N → 0 over 30 s, `gracefulRampDown` 10 s | Same schedule. VU *i* starts at ramp·i/N; the highest-numbered VUs stop first; an iteration still running 10 s after its VU stops is interrupted and not counted |
| `if (__ITER === 0) sleep(between(0, 5))` | Same |
| `GET /feed`; on non-200: `sleep(3–7)`, end the iteration | Same |
| pick a random post from `posts`, `sleep(3–7)`, `GET /posts/:id`, `sleep(3–8)` | Same. Post ids are scanned from the raw body (`{"id":`), with no JSON parse |
| 15 % `POST /posts/:id/like`, 2 % `POST /posts` with `"<user> says hi at <ISO> (VU n, iter i)"` | Same body format; token = `tokens[(VU-1) % len]` |
| one keep-alive connection per VU, 60 s request timeout | Same. Like Go's HTTP client, a request that fails on a *reused* idle connection is retried once on a new one |
| `http_req_duration` = send → last byte, excluding connect | Same; connect time is reported separately |
| `http_req_failed` = status 0 or ≥ 400 | Same |
| thresholds `p(95)<500`, `p(99)<1000`, `rate<0.01` over the whole run | Same, plus a hold-window-only line |

Percentiles come from a log-linear histogram, accurate to within ~1.6 %.

## Extras that k6 doesn't report (asked for in issue #62)

- requests per second overall and per endpoint, with failures per endpoint
- connection attempts per second, connect failures (including running out of local ports), connect-time percentiles, timeouts, keep-alive retries, max concurrently open sockets
- bandwidth in each direction
- the generator's own CPU and peak RSS (KB per VU)
- `--json FILE` for scripting

## Source ports

One source IP can open at most ~64k connections to one `ip:port`. `--source-ips 10.0.0.5,10.0.0.6` (or a range such as `127.0.0.1-127.0.0.8`)
spreads VUs across local addresses. For loopback targets this happens automatically, with one `127.0.0.x` per 25k VUs.
kload picks each source port itself (an explicit `bind(ip, port)`) from one cursor per IP that every thread shares.
`bind(ip, 0)` makes the kernel scan for a free port, and that scan got so slow at ~24k sockets per IP that it stalled
the event loops.

## Self-check

A generator that stalls makes the server look slow. kload times each of its own event-loop passes and prints
`self-check: passes over 100ms: N` (also `gen_loop_stalls` in `--json`). If N isn't 0, don't trust the latency numbers.

## Validation against k6 (same server, same box, back to back)

| 25,000 VUs, 30 s ramp + 60 s hold | k6 v2.3.0 | kload |
|---|---|---|
| requests | 259,275 | 259,166 |
| p95 / p99 | 0.68 / 2.30 ms | 0.7 / 2.5 ms |
| generator peak RSS | 7.56 GB | 0.20 GB |
| generator CPU | 110 s | 27 s |

| 120,000 VUs, 60 s ramp + 60 s hold | k6 (5 processes) | kload (1 process) |
|---|---|---|
| requests | 1,431,947 | 1,415,301 |
| generator peak RSS | 35.0 GB | 0.33 GB |
| generator CPU | 668 s | 39 s |

When the server is saturated, kload reported much worse tails than k6 (p99 1155 ms vs 384 ms). A plain `curl /feed`
probe running alongside saw the same 1–2 s stalls, so the server really was stalling (they turned out to be SQLite
write-lock waits).
