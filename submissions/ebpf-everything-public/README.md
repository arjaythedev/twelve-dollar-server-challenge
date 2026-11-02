# ebpf-everything-public: the whole server but SQLite, in the kernel

Everything the HTTP server does — accept bookkeeping, HTTP/1.1 framing, routing, input validation, HS256 JWT
verification, error and response formatting — runs inside **eBPF programs on the socket layer**. A tiny userspace
**shim** does one thing: run SQLite. No framework, (almost) no userspace.

**Please read this first.** This is a **novelty / rules-probe entry.** It now **runs end-to-end and answers every
endpoint correctly** on kernel 6.8 (see [status](#status-it-runs) below) — but it still **cannot load under the
challenge's runtime contract**, and that one hard blocker is why it needs an organizer ruling rather than a score:

- **Privileges (the hard blocker).** The contract runs the server unprivileged with only `CAP_NET_BIND_SERVICE`, and
  Ubuntu 24.04's stock `kernel.unprivileged_bpf_disabled=2` means it cannot load a single BPF program. Loading
  `sk_skb`/sockmap/sockops needs `CAP_BPF + CAP_NET_ADMIN` — a privilege escalation the contract withholds.
  `install.sh` does **not** grant it. So under the stock contract this entry does not come up. The question for you:
  is an "everything in eBPF" entry admissible at all, given it must be granted BPF capabilities to load?

The other real result: the entire HTTP + HS256-JWT + strict-JSON + routing logic **passes the kernel 6.8 verifier**
as a set of `sk_skb`/`sockops` programs — it started at >1,000,000 instructions / a 640-byte stack against the 1M /
512-byte limits (see [§3](#3-what-it-took-to-pass-the-verifier)).

## Status: it runs

Granted `CAP_BPF` on a kernel-6.8 VM (Ubuntu 24.04, 6.8.0-117), the server comes up and serves correct responses.
A local functional check — the spec's five endpoints plus its full error/auth table — **passes 20/20, reliably**
(5 of 5 back-to-back runs on a fresh DB), including requests split across TCP segments (see below):

```
GET /health            200  {"status":"ok","db":"ok","uptime_s":1}
GET /feed              200  20 newest posts, created_at DESC, id DESC, with author + like_count
GET /posts/:id         200  the post object   | /posts/abc,/posts/0 -> 400 invalid post id | /posts/99999 -> 404
GET /nope              404  not found
POST /posts (no tok)   401  missing bearer token   | bad sig / expired -> 401 invalid or expired token
POST /posts/abc/like   401  (auth checked before id, per spec)
POST /posts (valid)    201  {"post":{...,"author":"<token username>",...}}
  empty body -> 400 body is required | >500 chars -> 400 | bad JSON -> 400 malformed JSON body
POST /posts/:id/like   201 first time (already_liked:false), 200 on repeat (already_liked:true)
keep-alive             multiple requests on one connection
```

This is not the official `test/test.sh` (that needs the full 250 MB seed and the BPF caps in CI, which CI does not
grant), but it exercises the same behaviours against a small seed built from `schema.sql`. SQLite runs with WAL +
`synchronous=NORMAL`, one transaction per batch, every response built from a read during that batch, nothing cached
across batches; `src/engine.c` reads the B-trees directly and is reused unchanged from my userspace entry.

**Segmentation is handled.** A request split across TCP segments — headers in one skb, body in the next, which a
real client/Nginx may do at any time — is reassembled by the `sk_skb` **stream parser** (`on_parse` returns the full
message length = headers + `Content-Length`, so the kernel buffers until the whole message arrives before running
the verdict). A probe of 40 deliberately segmented `POST`s returns 40/40 `201`, and the shim stays up. (An earlier
revision got this wrong: it ran verdict-only with no reassembly, and a segmented request hung and wedged the shim —
fixed in §4.)

What is **not** solved: there is no connection-close handling (the `closes` ring buffer is wired but not drained),
so held client fds leak under churn. So the functional behaviour is right and robust to segmentation, but sustained
load / connection churn is untested and would leak — this is still a prototype, not a hardened server. And it is not
the official `test/test.sh` (that needs the 250 MB seed and BPF caps in CI).

Getting here from "does not deliver" meant fixing bugs in the **shim/BPF glue** (the in-kernel logic was already
correct) — see [§4](#4-what-actually-blocked-delivery-and-the-fixes). The earlier revision claimed `sk_skb`/sockmap
(and its stream parser) "does not deliver on 6.8"; that was wrong — it was these bugs, and the parser works fine.

### Performance (a datapoint, not the scored metric)

Single-connection, sequential, keep-alive GETs, on the **arm64** dev VM (NOT the x86 scored box, NOT a load test):
**~110 µs/request, ~9,000 req/s** for both `/health` and `/feed` (with the stream parser; ~75 µs / ~13k req/s
verdict-only, before segmentation handling). That is only after setting `TCP_NODELAY` on the accepted client
sockets — without it a 40 ms delayed-ACK stall capped it at ~24 req/s. The scored metric is
concurrent connections *held*, which I have **not** measured — that belongs on the x86 broker and I did not run it
here. Expectation is unchanged: **no throughput win** over a good userspace entry (valid requests still cross into
userspace for SQLite; the parse/JWT CPU just moves into softirq on the same single vCPU), plus extra per-socket
kernel memory for the psock/sockmap state.

## Design

```
            client TCP socket (in the `clients` sockhash, keyed by socket cookie)
                 │  bytes
                 ▼
  (the shim adds each accepted socket to `clients` after accept; the on_sockops program is unused in this build)
  sk_skb  on_parse (parser)     returns the full message length (headers + Content-Length) so the kernel reassembles
                                a whole request before the verdict runs — handles requests split across TCP segments
  sk_skb  on_request (verdict)  request line + routing. /feed /health /posts/:id → job. 4xx → error job.
                 │                 authenticated routes: Bearer check, then tail-call →
  sk_skb  on_auth               HS256: base64url + HMAC-SHA256 (bpf_loop, no HMAC helper on 6.8) + strict-JSON
                 │                 header/alg + claims + exp/nbf compare. ok → tail-call →
  sk_skb  on_job                like / create (+ request-body JSON validation, trim, 500-char limit) → job record
                 │  job record (src/proto.h), redirected into a unix socket
                 ▼
  shim (userspace)             the ONLY userspace code: batch-reads jobs → one SQLite transaction
                 │                 (src/engine.c, direct B-tree reads) → raw JSON bodies
                 │  reply datagram (cookie + status + body), written to a TCP-loopback reply socket
                 ▼
  sk_skb  on_reply (verdict)   prepends the status line + headers, redirects to the client socket by its cookie
```

The request path is split into three tail-called programs (`on_request` → `on_auth` → `on_job`) so each stage gets a
fresh 512-byte verifier stack; that split is the only way the JWT work fits (see §3).

## 1. Privileges: the contract forbids loading

Measured on a stock arena VM (Ubuntu 24.04.5, kernel 6.8.0-117):

| How the server is run | create any BPF map / load any program |
|---|---|
| As the harness runs it (`deploy` user, `CAP_NET_BIND_SERVICE`) | **EPERM** — not even a socket filter |
| Binary given `cap_bpf,cap_net_admin+ep` by root (a `setcap` in install.sh) | OK |
| root | OK |

`probe/bpfprobe.c` is the check. `kernel.unprivileged_bpf_disabled=2` blocks every `bpf()` create/load without
`CAP_BPF`; `sk_skb`/sockmap/sockops also need `CAP_NET_ADMIN`. Granting those to the server binary is a privilege
escalation the contract deliberately withholds, so a real submission can't do it without an organizer ruling — which
is what this entry asks for. `install.sh` therefore does not grant the capability.

## 2. Spec conformance of the in-kernel logic

The BPF programs implement the spec's routing, error, and auth rules directly: the five endpoints, the exact error
table (auth before id; the full set of 400/401/404 messages), HS256-only JWT with `exp`/`nbf` verification,
strict-JSON claim parsing, body trimming to 500 **code points**. The `author` in a created post is the token's
`username`. The §Status check above confirms all of these against a running instance.

## 3. What it took to pass the verifier

The request program started at **>1,000,000 instructions** (the 6.8 limit) and a **640-byte call stack** (limit 512).
In order of impact:

- **base64url decode** was the biggest cost — a variable output offset plus a branchy decoder exploded the state
  count. Rewritten branchless: a `.rodata` lookup table, fixed `i*3` output stride, validity as an OR accumulator,
  output length computed arithmetically.
- **All loop-carried state lives in a per-CPU scratch map, not in registers** — loop-carried scalars make the
  verifier unroll and blow the 8192 jump-sequence limit.
- **Heavy routines are `__noinline` subprograms** verified once; `j_step` (the JSON byte step) **must** stay
  `__noinline` or the big switch re-explodes the loop.
- **Tail calls for the 512-byte stack limit** — the request path is three tail-called programs, and `verify_jwt` is
  itself split into `jwt_sig` and `jwt_claims` so the HMAC frame and the claims frame never share a call chain.

All five programs load on kernel 6.8.0-117 (`bpftool prog loadall bin/http.bpf.o` → OK).

## 4. What actually blocked delivery, and the fixes

The in-kernel pipeline was correct all along; bugs in the shim/BPF glue stopped it end-to-end. All are fixed in this
revision:

1. **The per-CPU scratch map was never sized for the box.** `scr` is a global array indexed by
   `bpf_get_smp_processor_id()*2 + level`; it ships with a placeholder of 2 entries and the shim is meant to resize
   it to `2 × possible CPUs` before load. It didn't — so `grab()` returned NULL (and the verdict dropped the
   request) on every CPU but 0. The shim now calls `bpf_map__set_max_entries`.
2. **`on_reply` was attached to its sockmap *after* its socket was inserted.** sockmap wires a socket's verdict from
   the map's attached programs at insert time, so the reply socket got no verdict and `on_reply` never fired. The
   attach now happens before the insert (as it already did for the `clients` sockhash).
3. **The reply socket was AF_UNIX**, whose verdict did not deliver here; it is now a TCP-loopback pair, and the shim
   drives `on_reply` with a `recv()` nudge on it per batch — the same mechanism that drives `on_request` on the
   client sockets.

4. **Framing was single-segment, which hung and wedged the shim on a split request.** Running verdict-only, the
   verdict saw each skb separately, and `send_job` tagged the job with the full request length while redirecting only
   the first skb's bytes — so on a segmented request the shim waited forever for bytes that never came, poisoning its
   job buffer for every later request. Fixed two ways: the client sockets now use the `sk_skb` **stream parser**
   (`on_parse`) so the kernel reassembles a whole message before the verdict runs, and `send_job` now tags the job
   with the bytes actually redirected (`skb->len`). 40/40 deliberately segmented `POST`s now return `201`.

Also fixed: the created-post `author` came back empty because the request-body JSON parse reset the shared parser's
username-length field; the length is now saved right after the JWT claims parse.

Honest remaining gaps (fine for the functional check, not for a sustained load run):

- **No connection-close handling.** The `closes` ring buffer is wired but not drained, so held client fds leak under
  churn — the reason sustained load is untested. This is the main thing between the prototype and a robust server.
- **The reply verdict is driven by a per-batch `recv()` nudge** rather than firing on its own — a syscall per batch.
  It is reliable (it is how the request side is driven too), but a production version would avoid it.

## Files

- `src/proto.h` — records shared between the BPF programs and the shim.
- `src/http.bpf.c` — the five programs (sockops, request/auth/job verdicts, reply verdict).
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
