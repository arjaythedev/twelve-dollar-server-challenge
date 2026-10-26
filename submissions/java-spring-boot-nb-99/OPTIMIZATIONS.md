# Possible future optimizations

None of these is measured yet. Check each change with the 42-check suite on a
fresh database, then with `bench/load.js` at a fixed user count and in a full
search. Compare CPU per request, p95, p99, and errors over repeated runs, and read
a profile before naming a bottleneck. Do not keep a faster run that skips work or
returns errors.

Profiles of the WebFlux version put about 90% of Java CPU in Spring WebFlux,
Reactor, and Netty, so the framework rows come first.

| Change | What to measure | Cost or risk |
| --- | --- | --- |
| Functional WebFlux routes (`RouterFunction`) instead of annotated controllers | CPU per request in handler mapping, reflection, and argument resolution | Error handling and status codes must stay identical. |
| Serve the five routes on Reactor Netty or Netty without Spring's web layer | CPU per request and allocation rate | More HTTP code to own: routing, header parsing, keep-alive, and error responses. |
| Young-generation size and GC settings | Young pause times, which averaged 79 ms under load, and allocation rate | A larger young generation takes memory from SQLite's page cache and the OS. |
| Number of reader connections and SQLite threads | Read queue time, CPU, and tail latency | Extra threads cannot add CPU on one vCPU, and each connection has its own page cache. |
| SQLite page cache and `mmap_size` | Read I/O, page faults, resident memory, and feed latency | The JVM and the OS also need part of the 2 GiB. |
| WAL checkpoints outside the writer's request path | WAL growth, checkpoint time, and write tail latency | Delayed checkpoints need more disk space and can cause later latency spikes. |
| Group several writes into one transaction | Commit cost, queue delay, and p99 write latency | Writes are about 8% of requests. Every response must still wait for its commit. |
| Combine the like's existence check and insert into one statement | SQL calls and write latency | Must still tell a missing post from a repeated like: 404, 200, and 201. |
| Fewer allocations in authentication and the remaining JSON responses | Allocation profile and CPU for likes, new posts, and errors | Must keep exact key order, escaping, `exp` checks, and per-request signature verification. |

WAL with `synchronous=NORMAL` or stronger stays. Response caches, query-result
caches, remembered JWT verifications, in-memory table copies, external services,
schema changes, benchmark detection, and weaker durability are out.
