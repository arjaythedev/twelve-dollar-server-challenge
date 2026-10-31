# ebpf-everything-public: the whole server but SQLite, in the kernel

Everything the HTTP server does — accept bookkeeping, HTTP/1.1 framing, routing, input validation, HS256 JWT
verification, error and response formatting — runs inside **eBPF programs on the socket layer**. A tiny userspace
**shim** does one thing: run SQLite. No framework, (almost) no userspace.

**Please read this section before reviewing.** This is a **novelty / rules-probe entry, submitted so an organizer
can rule on it — not a clean, passing submission.** Two things block it, both documented honestly; neither is hidden:

1. **Privileges — it cannot load on the box as the harness runs it.** The contract runs the server as an
   unprivileged user with only `CAP_NET_BIND_SERVICE`, and Ubuntu 24.04's stock `kernel.unprivileged_bpf_disabled=2`
   means it cannot load a single BPF program. Loading `sk_skb`/sockmap/sockops needs `CAP_BPF + CAP_NET_ADMIN` — a
   privilege escalation the contract withholds. `install.sh` does **not** grant it. So this entry **does not run**
   under the stock contract, and I am not asking you to tune the box for it. The question for you is whether an
   "everything in eBPF" entry is admissible at all given that constraint.
2. **Runtime — no end-to-end response yet.** With the capability granted on a dev VM, all five BPF programs load and
   the request pipeline executes in-kernel (parse → route → job, confirmed by tracing), but I could not get `sk_skb`
   / sockmap to deliver client data to the verdict program reliably on my kernel-6.8 test VM. **So there is no
   42/42, no memory, no CPU, no users-held number** — I have none and I won't invent any. (My VM is arm64; the box
   is x86_64, and `sk_skb`/sockmap can differ by arch, so this is not yet a firm conclusion about the mechanism.)

What *is* a real result: the entire HTTP + HS256-JWT + strict-JSON + routing logic **passes the kernel 6.8 verifier**
as a set of `sk_skb`/`sockops` programs. That is the hard, interesting part of this entry (see
[§3](#3-what-it-took-to-pass-the-verifier)).

## Design

```
            client TCP socket (in the `clients` sockhash, keyed by socket cookie)
                 │  bytes
                 ▼
  sockops on_sockops            adds each new connection to `clients` at PASSIVE_ESTABLISHED
  sk_skb  on_parse              stream-parser: request length (headers + Content-Length) for pipelining
  sk_skb  on_request (verdict)  request line + routing. /feed /health /posts/:id → job. 4xx → error job.
                 │                 authenticated routes: Bearer check, then tail-call →
  sk_skb  on_auth               HS256: base64url + HMAC-SHA256 (bpf_loop, no HMAC helper on 6.8) + strict-JSON
                 │                 header/alg + claims + exp/nbf compare. ok → tail-call →
  sk_skb  on_job                like / create (+ request-body JSON validation, trim, 500-char limit) → job record
                 │  job record (src/proto.h), redirected into a unix socket
                 ▼
  shim (userspace)             the ONLY userspace code: batch-reads jobs → one SQLite transaction
                 │                 (src/engine.c, direct B-tree reads) → raw JSON bodies
                 │  reply datagram (cookie + status + body), back on a unix socket
                 ▼
  sk_skb  on_reply (verdict)   prepends the status line + headers, redirects to the client socket by its cookie
```

The shim runs SQLite with WAL + `synchronous=NORMAL`, one transaction per batch, every response built from a read
during that batch, nothing cached across requests. `src/engine.c` (`#include`s the pinned SQLite amalgamation and
reads B-trees directly via `sqlite3BtreeTableMoveto`/`IndexMoveto`/`Insert`) is reused unchanged from my fast
userspace entry; only the HTTP/JWT/routing layer moved into the kernel.

The request path is split into three tail-called programs (`on_request` → `on_auth` → `on_job`). That is not for
speed — it is the only way the JWT work fits under the verifier's 512-byte stack limit; see below.

## 1. Privileges: the contract forbids it

Measured on a stock arena VM (Ubuntu 24.04.5, kernel 6.8.0-117):

| How the server is run | create any BPF map / load any program |
|---|---|
| As the harness runs it (`deploy` user, `CAP_NET_BIND_SERVICE`) | **EPERM** — not even a socket filter |
| Binary given `cap_bpf,cap_net_admin+ep` by root (a `setcap` in install.sh) | OK |
| root | OK |

`probe/bpfprobe.c` is the check; it was run inside a `systemd-run` unit shaped like the harness's.
`kernel.unprivileged_bpf_disabled=2` blocks every `bpf()` create/load without `CAP_BPF`; `sk_skb`/sockmap/sockops
also need `CAP_NET_ADMIN`. Granting those to the server binary is a privilege escalation the contract deliberately
withholds. **So a real submission can't do this without an organizer ruling** — which is what this entry asks for.
`install.sh` therefore does not grant the capability.

## 2. Spec conformance of the in-kernel logic

The BPF programs implement the spec's routing, error, and auth rules directly: the five endpoints, the exact error
table (auth checked before id; `invalid post id`/`post not found`/`missing bearer token`/`invalid or expired
token`/`invalid token payload`/`malformed JSON body`/`body is required`/`body must be at most 500 characters`),
HS256-only JWT with `exp` verification, strict-JSON claim parsing, body trimming to 500 **code points**. SQLite is
run exactly as the spec's reference queries require (newest-20 feed, per-post `like_count` as of the request,
`INSERT … RETURNING`, `ON CONFLICT DO NOTHING` for likes), one transaction per batch, nothing cached.

Because of blocker #2 this conformance is **not yet verified end-to-end by `test/test.sh`** — the data plane doesn't
deliver on my VM. The HMAC-SHA256 and strict-JSON were cross-checked against known JWT vectors during development.

## 3. What it took to pass the verifier

The request program started at **>1,000,000 instructions** (the 6.8 limit) and a **640-byte call stack** (limit 512).
Getting it to load, in order of impact:

- **base64url decode** was the single biggest cost — a variable output offset plus a branchy decoder exploded the
  state count. Rewritten branchless: a `.rodata` lookup table, fixed `i*3` output stride, validity as an OR
  accumulator, output length computed arithmetically (not accumulated in the loop).
- **All loop-carried state lives in a per-CPU scratch map, not in registers.** The verifier treats map memory as
  opaque and merges states across the back-edge; loop-carried scalars make it unroll and blow the jump-sequence
  limit (8192).
- **Heavy routines are `__noinline` subprograms** (`json_scan`, `b64_decode`, `hmac`, `num_cmp`, …) verified once;
  `j_step` (the JSON byte step) **must** stay `__noinline` or the big switch re-explodes the loop.
- **Every byte loop is capped** well under 8192 and indices are masked; where clang dropped a mask the verifier
  could see, an inline-asm `BOUND()` forces it.
- **Tail calls for the stack limit.** Splitting the request path into three tail-called programs
  (`on_request`/`on_auth`/`on_job`) gives each stage a fresh 512-byte budget; `verify_jwt` is itself split into
  `jwt_sig` and `jwt_claims` so the HMAC frame and the claims frame never sit on one call chain.
- `put_u32` writes digits straight into the destination (map memory) — a stack temp indexed by a variable is
  rejected ("R1 subtraction from stack pointer").

All five programs load on kernel 6.8.0-117 (`bpftool prog loadall bin/http.bpf.o` → OK).

## 4. Runtime blocker: sk_skb delivery on 6.8

With the binary granted `CAP_BPF`, the shim loads everything, wires the sockmap/sockhash/prog-array, listens, and
runs. Tracing (`bpf_printk`) confirms the **in-kernel pipeline is correct**: a request produces
`parse → on_request → send_job(route=…)`. But getting the kernel to invoke the verdict on client data was not
reliable on my VM:

- **sockops-added sockets never deliver to the verdict.** `on_sockops` adds the connection at `PASSIVE_ESTABLISHED`
  (before any data), but `sk_psock_verdict_data_ready()` early-returns while `sk->sk_socket` is NULL — which it is
  until userspace `accept()`s — so data that arrives before the shim accepts is dropped by the hook, and buffered
  data is never replayed.
- **userspace-added sockets** (adding the accepted fd to the sockhash) do install the hook, but on this VM the
  verdict only ran when I "nudged" `data_ready` with a `MSG_PEEK recv`, and then it saw a **zero-length skb**.
- The **stream-parser** path (`on_parse` + `on_request`) never fired on data arrival at all on this VM.

So **no response is produced end-to-end**, and there are no 42/42, memory, CPU or users-held numbers. My test VM is
arm64 (on Apple Silicon); the scored box is x86_64, and sockmap/`sk_skb` behaviour can differ by arch, so this needs
re-testing on an x86 6.8 box before any firm conclusion about the mechanism.

The accept model also holds every client fd and omits connection-close handling (the `closes` ring buffer is wired
but not drained), so even if delivery worked it would leak fds under churn.

## Expected outcome, if it ran

No speed win over a good userspace entry: valid requests still cross into userspace (batched), and the parsing/JWT
CPU moves into softirq on the same single vCPU. It would also add per-socket kernel memory (psock + sockmap state).
The point of this entry is the "everything in eBPF" conceit and the rules question, not the score.

## Files

- `src/proto.h` — records shared between the BPF programs and the shim.
- `src/http.bpf.c` — the five programs (sockops, parser, request/auth/job verdicts, reply verdict).
- `src/shim.c` — loader + wiring + SQLite batch loop (self-contained SHA-256 for the HMAC midstates; no OpenSSL).
- `src/engine.c`, `src/engine.h`, `src/unit_sqlite.c` — the SQLite side (unity TU, direct B-tree reads).
- `build.sh` / `install.sh` / `start.sh` — the challenge's three scripts. `install.sh` does **not** grant BPF caps (§1).
- `probe/bpfprobe.c` — the privilege probe behind the table in §1.

## Build / run (on a 6.8 box, as root or with CAP_BPF — see §1; it will NOT load under the stock contract)

```bash
bash build.sh                 # bin/http.bpf.o + bin/shim
sudo setcap cap_bpf,cap_net_admin,cap_net_bind_service+ep bin/shim   # OUT OF CONTRACT
SQLITE_PATH=/path/to/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80 ./start.sh
```
