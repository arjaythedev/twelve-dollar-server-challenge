# Possible future optimizations

Keep the first draft as a correctness baseline. Measure changes on the challenge's
one-vCPU, 2 GiB VM before retaining them. The Compose limits do not reproduce the
whole benchmark machine or its remote load generator.

## Measure before changing settings

Run the repository's correctness suite against a fresh database before each
candidate. Then use `bench/load.js` from a separate load-generator machine with
the specified warm-up, search, and five-minute confirmation. Record the commit,
JDK, JVM flags, SQLite pragmas, connection and thread counts, workload, user count,
run count, latency percentiles, error rate, CPU, and total memory. Compare repeated
runs and report their spread. Inspect profiles and counters before naming a
bottleneck. Do not accept faster runs that skip work or return errors.

## Candidates

| Change | What to measure | Cost or risk |
| --- | --- | --- |
| Change the JDBC pool size | Connection wait time, writer contention, latency, and memory | More connections do not create more SQLite writers and each connection has a page cache. |
| Tune Tomcat's request threads or enable Java virtual threads | CPU, thread memory, blocked time, throughput, and tail latency | Extra concurrency can increase the SQLite wait queue on one CPU. |
| Tune SQLite page cache and `mmap_size` | Read I/O, resident memory, page faults, and feed latency | The JVM, OS, and Nginx also need part of the 2 GiB budget. |
| Tune WAL checkpoints | WAL growth, checkpoint time, and write tail latency | Delayed checkpoints need more disk space and can cause later latency spikes. |
| Reuse prepared statements per connection | SQL preparation CPU and allocation rate | Statement lifetime and concurrent ownership need care. Cache plans, never results. |
| Separate readers from a serialized writer connection | Busy failures, queue time, and read latency during writes | Adds scheduling and connection ownership code. Each response must wait for commit. |
| Group several writes into a transaction | Commit cost, queue delay, and p99 write latency | Adds cancellation and failure handling. Respond only after the shared transaction commits. |
| Compare other equivalent feed and like-count queries | Query plans and elapsed SQL time on fresh and grown data | Cannot add indexes or store derived counts. Every request must read current SQLite data. |
| Combine the like's existence check and insert into one statement | SQL call overhead and write latency | Must distinguish a missing post from a repeated like and preserve 404, 200, and 201 responses. |
| Raise Tomcat's maximum requests per keep-alive connection | Upstream reconnects and CPU | The default closes a connection after 100 requests. Compare settings with Nginx before removing the limit. |
| Tune JVM heap and garbage collection | GC pauses, allocation rate, RSS, and OOM risk | A larger heap can take memory from SQLite and the OS. The heap limit does not bound native memory. |
| Reduce JSON or authentication allocation | Allocation profiles and CPU time after SQL bottlenecks are ruled out | Must preserve exact JSON ordering, escaping, expiration checks, and per-request signature verification. |
| Serve directly without Nginx | Whole-machine CPU and behavior with about 15,000 open keep-alive connections | Must bind the required host and port and handle the full connection count itself. |

WAL with `synchronous=NORMAL` or stronger is non-negotiable. Do not add response
caches, query-result caches, remembered JWT verifications, in-memory table copies,
external services, schema changes, benchmark detection, or weaker durability.
