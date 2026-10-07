# Tests

Tests in C, beyond the contest's `test/test.sh`. They use only libc and the compiler that
`install.sh` installs: no curl, jq, openssl, Python or other runtimes. `build.sh` does not build or
run them.

```bash
bash build.sh            # fetches the pinned SQLite and liburing sources, builds bin/server
make -C tests check      # builds the tests into tests/build/ and runs them (about 2 minutes)
```

Each program starts its own server on a free loopback port with a fresh copy of `seed/feed.db`, prints
one `ok` or `FAIL` line per check, and exits nonzero if any check failed. `SERVER` and `SEED` override
the paths. `lifecycle` needs a hard file-descriptor limit of at least 15,064 (`ulimit -Hn`).

| Program | What it checks |
|---|---|
| `parser` | The request parser in-process under AddressSanitizer and UBSan. Each of 13 requests (malformed line endings, conflicting lengths, chunked bodies) is split in two at every byte. |
| `api` | Strict JSON and UTF-8. Unicode length limits and trimming. Escaping round trips for bodies and usernames. JWT claims (`exp` required, finite and checked to the sub-second; `nbf`; `alg` exactly HS256; `sub` and `username` types). Auth before id. Foreign keys for unknown users. 16 concurrent likes. 12 concurrent creates in feed order. 300 rounds of feed and post reads matching under random writes. |
| `http` | Requests split byte by byte and mid-pipeline. Mixed-case headers. Chunked bodies with extensions and trailers. Ambiguous framing. `100 Continue`. HTTP/1.0 and `Connection: close`. Oversized heads. Aborted and reset uploads. Responses larger than a buffer. Backpressure: a client pipelines 20,000 requests without reading, server memory must stay flat, and every response must arrive once it reads. The 30-second deadline for stalled requests and non-reading clients, which leaves idle keep-alive connections open. |
| `commit` | Uses `build/commit-server`, the server linked with the commit hook in `commit_fault.c`; `bin/server` never contains it. A failed group commit answers 500 and rolls back. A read pipelined after the failed write is dropped. Concurrent reads never see rolled-back rows. A 201 is not sent before its commit finishes. |
| `lifecycle` | Acknowledged writes survive SIGKILL. 15,000 keep-alive connections all answer again after 66 idle seconds. |
| `loadgen` | Not a test: the load generator for capacity runs. Each user follows the loop of `bench/load.js` on its own keep-alive connection. `THINK=0` measures maximum throughput. See the comment at the top of `loadgen.c`. `make -C tests` builds it, and `check` does not run it. |

## Sources

Besides our own checks, the cases come from other submissions' public test suites: PRs 3, 4, 14 and 18
(tbsvttr) and PR 19 (Scooter1337) of the contest repository, surveyed on 2026-10-07. They were rewritten
in C here. One expectation was not adopted: PR 19 treats `POST /posts/like` as a like with a missing
id (401, then 400). SPEC.md routes only `/posts/:id/like`, so it is an unknown path (404).
`POST /posts//like` (an empty id) gets 401, then 400 `invalid post id`.

These are correctness checks, not the scored k6 benchmark.
