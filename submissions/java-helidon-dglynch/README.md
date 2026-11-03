# Java 27 + Helidon 27

|                 |                                                                       |
|-----------------|-----------------------------------------------------------------------|
| Language        | Java 27                                                               |
| Framework       | Helidon 27                                                            |
| SQLite driver   | Xerial `sqlite-jdbc`                                                  |
| JSON            | Mix of Helidon and manual string concatenation                        |
| JWT             | Java crypto APIs (this part of the code is mostly AI slop)            |
| Build           | Gradle: dependency versions are pinned in `gradle/libs.versions.toml` |
| Nginx or direct | Direct: serves `0.0.0.0:80`                                           |

## Implementation and optimizations

- **Helidon web server.** Helidon handles HTTP, and is lighter than some of the alternatives. Virtual threads avoid the
  need for Nginx. I'd never used this library before and I wanted to try it out.
- **Prepared SQLite statements.** A dedicated read connection and a dedicated write connection each prepare their
  statements once and reuse them across requests. Reads are serialized on a single reader executor and writes are
  serialized on a single writer executor.
- **Batched durable writes.** The writer drains queued "post" and "like" operations into a transaction batch. It commits
  before completing the waiting requests, so successful write responses are sent only after commit.
- **SQLite settings.** Connections use WAL mode with `synchronous=NORMAL`, foreign-key enforcement, a 64 MB page
  cache, and in-memory temporary storage.
- **Fixed Java heap size.** The 1.25 GB fixed pre-touched heap leaves enough memory for the operating system, the JVM
  itself, SQLite native allocations, and other processes. The extra time spent pre-touching the heap is spent during the
  startup phase, so it does not affect benchmark throughput. No time is spent growing or shrinking the heap during the
  benchmark run.
- **Sealed interfaces, records, and data-oriented programming.** I'm not sure whether these actually helps performance,
  but I'd never used them before and wanted to try them out.

## License

MIT, under the repo's [license](../../LICENSE).
