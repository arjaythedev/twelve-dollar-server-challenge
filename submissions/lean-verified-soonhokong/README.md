# Lean 4: the handler the theorems are about is the one that runs

The request handler is Lean, compiled by the Lean compiler, and the theorems in `Feed/Proofs/` are
about its definitions. It is written once, generic in its storage operations: the build checks the
proofs on a model of the tables and compiles the same code against SQLite. Lean runs the control
flow of every batch and every request; C leaves under it do HTTP framing, routing, the JSON of
bodies and tokens, HMAC-SHA256 and base64url, and the bytes of the two read responses. Each leaf is
a Lean definition that `@[extern]` replaces with C in the binary, and the build tests the C against
the definition; the proofs unfold the definitions of 15 of the 22 leaves and take the results of
the other 7 as given. The C runtime (SQLite, the storage operations, the leaves, the io_uring
transport) is trusted; the build tests the storage operations against the model and each leaf
against its definition.

| | |
|---|---|
| Language | Lean 4.37.0 nightly 2026-10-30 (handler and proofs), compiled to C by the Lean compiler; that C and the C runtime built by gcc `-O3 -march=native`, profile-guided |
| Framework | none |
| Database | SQLite 3.54.1 amalgamation in-process: reads of its B-tree pages through its pager, writes as prepared SQL |
| Transport | io_uring (Linux ≥ 6.1), one thread, HTTP/1.1 keep-alive, one multishot receive per connection (Linux ≥ 6.8; before 6.8 a poll of its direct descriptor) |
| **Nginx or direct** | **Direct**: serves `HOST:PORT` itself (`0.0.0.0:80`), no Nginx |
| Dependencies | Lean toolchain and SQLite, pinned by version and SHA-256 |

## What this entry contributes

This entry started from lean-derive-hargup #153, the first Lean entry (see Credits). What is new
here is that the handler that runs is compiled Lean, the theorems are about it and about the Lean
definitions of the C leaves it calls, and the build tests each leaf against its definition.

* **Lean on the request path.** Compiled Lean runs `Feed.batch` (`runBatch`: each batch's read
  phase, write phase, commit and replies), `readLoop` (pipelining, the rule that a write is its
  connection's first request in a batch, closing, the 400, 413 and 431 answers), `decide`
  (dispatch: the token first, then the id or the body), `authenticate` and `Jwt.verify` (exactly
  two dots, the constant-time comparison `ctEq`, `alg`, `exp` and `nbf` through `numGt`, `sub`
  through `positiveInt`, `username`), `postText` (the field's JSON kind, JavaScript's trim, 500
  code points), `feedLoop` (when the feed reads further), `postFound`, `perform` (the writes and
  their answers) and `respond`/`respondErr` (the status line, headers and bodies of errors, writes
  and `/health`). The transport calls `Feed.batch`, which is `runBatch` at `IO` by definition. The
  Lean is 1,527 lines (`Feed/` Bytes, Http, Json, Sha256, Jwt, Store, Handler and Server, and
  `Main.lean`), every function total (no `partial`). Part of it serves only as the definition of a
  C leaf: the server runs the C, and the self-test runs the Lean to check it. That part is the
  bodies of the 22 leaves (below) and the Lean only those bodies call, among them `renderPage`,
  `postFromPage`, `parseId`, the header scanners of `Http.parse`, `scanValue` and
  `Feed.Idle.pickList`.
* **Theorems about the code that runs.** The handler is generic in `Store m` (nine operations):
  the server runs `Store IO`, C over SQLite, and the proofs run `Store Model`, a state monad over
  the tables of `Feed/Spec.lean`. 124 `theorem`s in `Feed/Proofs/` (1,887 lines) and the
  `LawfulBEq ByteArray` instance, checked by `lake build` in `build.sh`, cover both reads' complete
  HTTP responses byte for byte, the writes, accepted tokens, routing, one request (`decide_spec`),
  a batch (`runBatch_spec`: a serial execution for any choice of the batches that may write, so the
  transport groups commits without a new proof) and the idle policy (10 theorems). The table below
  says what each states and what it takes as given.
* **C leaves with Lean definitions.** 22 C functions are `@[extern]` on Lean definitions whose
  bodies are their specifications. The proofs unfold 15 of those bodies (routing, the read
  responses, a post object, the byte operations other than `findHeadEnd`, the idle policy's
  choice); the theorems take the results of the other 7 as given (HTTP framing, the JSON of bodies
  and tokens, HMAC-SHA256, base64url, `findHeadEnd`). After linking, `build.sh` runs the
  self-test, which compares each leaf with its Lean definition on 20,000 random inputs and the
  storage operations with `Store Model` on a random SQLite file; any difference fails the build.
  The self-test's references are tied to the leaves' definitions by 22 `example`s and 8 lemmas in
  `Feed/Selftest.lean`, so the C is compared with the definitions themselves.
* **The idle policy as a Lean definition.** `Feed.Idle.pick` chooses which of the connections the
  transport offers to close, and `Feed/Proofs/Idle.lean` proves 10 theorems about it; the
  transport runs its C version, which the self-test compares with it. The policy is off by
  default: it runs only with `FEED_CONN_BUDGET` or `FEED_IDLE_MS` set, and `start.sh` sets
  neither. Closing idle connections has prior art (see Credits): c-iouring-moneebullah25 #228 and
  c-iouring-pavlyhalim #245 close keep-alives idle for 60 s and 90 s, #245's memory valve and
  c-iouring-public #158's memory floors, both off by default, close connections when memory runs
  low, and c-epoll-TheFirstIstari #64 closes its oldest idle ones under file-descriptor pressure.
  The Lean definition and its theorems are this entry's.
* **Compared with the earlier Lean entries.** lean-derive-hargup #153 and lean-c-iouring-loafecho
  #239 prove read plans against a Lean model, the feed plan under a data invariant the server
  checks; their READMEs say their C is tested and unproved, and neither runs Lean while serving
  (#153 generates its route matcher, error responses and serializers from Lean). This entry runs
  Lean while serving, at the cost of trusting Lean's compiler and runtime, and its theorems also
  cover routing and the response bytes (through the leaves' definitions), accepted tokens, the
  writes and the batch.

## What runs and what is proved

`Feed/Handler.lean` and `Feed/Server.lean` are generic in a storage monad (`Feed/Store.lean`):

```lean
class Store (m : Type → Type) where
  begin : m Bool
  feedPage : Nat → m Page   -- the first n posts in (created_at DESC, id DESC) order,
                            -- each with its author's name (if the user exists) and like count
  postPage : Nat → m Page   -- the post with this id, with its author and like count (or no row)
  health : m Bool           -- SELECT 1 succeeded
  endReads : m Unit
  postExists : Nat → m Nat
  insertLike : (uid pid : Nat) → m Nat
  insertPost : (uid : Nat) → (body : ByteArray) → m (ByteArray × Nat)
  commit : m Bool
```

* The server runs `Store IO`: each operation is a C function over SQLite (`runtime/db.c`).
* The proofs run `Store Model`: the same operations on the specification's tables, a state monad
  over lists of rows (`Feed/Model.lean`). Each C function is written to that one-line contract,
  except in three points. `db.c` takes the time of each insert, where the model gives every insert
  of a batch the same `clock`. SQLite's `CHECK (length(body) BETWEEN 1 AND 500)` counts the
  characters before the first NUL, so a body that starts with U+0000 is not inserted and is
  answered 500, where the model inserts it and answers 201. A post whose body or `created_at` is
  stored as a BLOB, which TEXT affinity allows, is left out of the C's pages, and a user whose
  username is a BLOB reads as a missing author, where the model takes their bytes; the seed and
  the API store only TEXT.

A `Page` is one byte array of text and one array of numbers, eight per row; `Page.view` reads a
row back as the post object it renders (none if the author row is missing). A read makes one
storage call, which gets the posts, their authors and their like counts together from the C side,
except that `GET /feed` reads again, twice as far down the order, while fewer than 20 of the posts
read have an author and the page was full (`feedLoop`); a like makes two calls (`postExists`, then
`insertLike`). The Lean side decides when the feed reads further; the response bytes are written by
C leaves (`respondFeed`, `respondPost`) whose Lean definitions say which rows are rendered and how:
the rows with an author, in the page's order, at most 20.

`Feed/Spec.lean` is the part of SPEC.md about the data and the answers, as Lean: tables as lists,
`ORDER BY created_at DESC, id DESC LIMIT 20` as a sort, the join and the like count as list
functions, the routes, the results of the five endpoints, the JSON the API sends and the bytes of
a response. It does not cover how requests are framed, the JSON the API reads, or the rules for
tokens and post bodies; the error messages are stated in `Proofs/Decide.lean` (`bs_err_body`,
`expected`), and the 503 body in no theorem. The theorems (checked by `lake build` in `build.sh`,
no `sorry`; `#print axioms` on all 124, run outside the build, lists at most `propext`,
`Classical.choice` and `Quot.sound`):

| Theorem | File | Statement |
|---|---|---|
| `feedResponse_correct` | `Proofs/Plan.lean` | `GET /feed`'s plan (a page of the newest posts with their authors and like counts, read again twice as far down the order while fewer than 20 of them have an author and the page was full) leaves the tables unchanged and appends `response 200 (feedJson db.feed) close` to the output, byte for byte: the status line, `Content-Type`, `Content-Length`, `Connection: close` when closing, then the body |
| `postResponse_correct` | `Proofs/Plan.lean` | `GET /posts/:id` leaves the tables unchanged and appends `response 200 (postJson v) close` when `db.getPost id = some v`, else `response 404 (errJson "post not found") close`, byte for byte |
| `feedFromPage_congr`, `respondFeed_congr`, `postFromPage_congr`, `respondPost_congr` | `Proofs/Plan.lean` | the response bytes built from a page depend on the page only through `Page.Same` (as many columns, every row reading back as the same post object), and so do the plan's decisions (`Page.authored`, the number of rows, `postFound`); the self-test checks `Page.Same` of every C page against the model's |
| `perform_like`, `perform_create` | `Proofs/Write.lean` | a like / a new post answers (status, body) and changes the tables as `DB.like` / `DB.create` |
| `Escape.decode_escape` | `Proofs/Escape.lean` | the JSON string escape is reversible: `Escape.decode` (in the same file: an RFC 8259 string body over bytes, with the escapes `escape` writes) gives back `s` from `escape s`; it does not check UTF-8 |
| `Jwt.verify_ok_sound` | `Proofs/Jwt.lean` | an accepted token's third part is `b64Encode` of `Hmac.mac` of its first two under the key passed to `verify`, its decoded header parses as an object whose `alg` is a string that decodes to `HS256`, and its `exp` is a number that `numGt` puts after now. It is stated in terms of the implementation's `b64Encode`, `b64Decode`, `Hmac.mac`, `parseTop`, `decodeString` and `numGt`, none of them tied to RFC 4648, RFC 2104, FIPS 180-4 or RFC 8259, and it is soundness only: a check that rejects every token meets it |
| `route_spec` | `Proofs/Route.lean` | routing on byte ranges of the input equals `Spec.route` on the path's bytes (5 endpoints, `:id` a positive integer in one segment); `route` is the C leaf `feed_route`, and the theorem is about its Lean definition |
| `decide_spec` | `Proofs/Decide.lean` | one request, for any `Req` (parsed or not): the response `expected` gives for `Spec.route` of its method and path bytes, or the write it asks for; the token first, then the id or the body; the snapshot unchanged. Whether the token and the post body are accepted, and the user and the text, are what the handler's own `authenticate` and `postText` return: the theorem takes them as given. `/health` is answered as after a `SELECT 1` that succeeded |
| `runBatch_spec` | `Proofs/Batch.lean` | a batch on the model is a serial execution: each connection gets the answers, from the batch's snapshot, to the requests `Http.parse` finds in its input, in order and at most 64 (a malformed one gets its error and ends the input), until a write, a close or an incomplete request, then the answer to its write when the write was its first request and the batch may write; the writes run in connection order, each on the database the earlier ones left, and the database ends as the snapshot with them applied. It states the response bytes and the final database (the input used and the flags the transport acts on are outside it) and takes `Http.parse`, `authenticate` and `postText` as given |
| `pickList_closed`, `pick_busy` | `Proofs/Idle.lean` | a candidate the idle policy closes is not flagged busy, and was idle for at least `minIdle` or past a timeout that is set |
| `count_pickList`, `pickList_none`, `pick_none` | `Proofs/Idle.lean` | without a timeout, the number closed is the smaller of the excess over the budget and the number of spare candidates (not busy, idle for `minIdle`); so none while the open connections are within the budget |
| `pickList_full` | `Proofs/Idle.lean` | while the open connections are within the budget, only candidates past a timeout that is set are closed |
| `pickList_longest` | `Proofs/Idle.lean` | if the candidates come in order of idle time, longest first, every one closed was idle at least as long as every spare one left open |
| `pickList_expired` | `Proofs/Idle.lean` | every offered candidate that is not busy and idle past a timeout that is set is closed |

The idle theorems are about the Lean definition, for whatever busy flags and order of candidates
it is given. In the transport the flags come from the C `conn_busy` and the order from the C merge
of the rings' lists, and neither is proved; the transport offers at most 2·`FEED_IDLE_PER_ITER` +
64 candidates per iteration (576 by default), and the theorems are about those.

The theorems about the API hold for every database with fewer than 20·2⁶³ posts: no assumption
about ids (the plans do not rely on rowids being distinct or positive) or about how ids and
timestamps relate. In the model every insert of a batch gets the same `created_at` (`DB.clock`),
and `begin`, `commit` and `SELECT 1` always succeed, so no theorem covers the 500 answers to
storage errors or the 503 of `/health`.

`runBatch_spec` is about `runBatch` on the model. The transport calls `Feed.batch`, which is
`runBatch` at `IO`: the same definitions, with the C storage operations in place of the model's and
the C leaves in place of their Lean definitions. No theorem relates the two; that they answer alike
is the argument of the trust base below. Within a batch, `runBatch_spec` is what makes group commit
and shared snapshots safe here: a connection's requests are answered in order, a write in a batch
is the first request of its connection in that batch, and nothing after a write is answered in the
same batch. That the order holds across batches, and that no answer leaves before the commit, rest
on the transport (below). `runBatch_spec` holds whether or not the batch may write (`writesOk`),
which is what lets the transport put writes into fewer batches than reads (Design) without a new
proof.

### The trust base

Not proved, and how it is checked instead where it is:

* **The C storage operations** (`runtime/db.c`): each implements the meaning of its `Store Model`
  operation, but for the three points above. `build.sh` runs `bin/server --selftest`, which writes
  a random database (ties in `created_at`, gaps in ids, posts whose author row is missing, a
  `users` table of three B-tree levels), runs `feedPage` on eleven sizes, `postPage` on 600 random
  and seven edge ids, and `postExists`, `insertLike` and `insertPost` on random arguments, before
  and after five rounds of random writes, and compares each result with the model's: a page by
  `Page.Same` (the congruence theorems prove that the responses built from such pages, and whether
  the feed reads further, are the same, so the C side may lay the text out in its own order). It
  gives the model's clock the time of each insert, its bodies hold no NUL and its text is stored as
  TEXT, so none of the three points shows there; it does not run `health`. It also compares the
  reads over the batch's pages with SQLite's cursors and record decoder (the rowid searches on
  every key of `users` and `posts` and on 4,000 random sets of keys, the in-order passes over both
  indexes, the feed's ids, the like counts of every post id and of random windows, runs of posts
  rows, the posts records' columns), again in a transaction, rolled back after, that adds 40,000
  posts and 200,000 likes so that those trees have three levels; it rolls back a transaction that
  grew the file and runs SQLite's integrity check. Any difference fails the build. No theorem puts
  these checks together with `runBatch_spec`: that `Feed.batch` answers as the theorem says when
  every C operation meets its contract up to `Page.Same` is argued from the handler being generic
  in its storage, and not proved.
* **The C leaves** (`runtime/leaves.c`, and `idle_pick` in `runtime/uring.c`): the byte operations
  of `Feed/Bytes.lean` (`appendEsc`, `appendNat`, `appendLitEsc`, `appendLitNat`, `appendSlice`,
  `appendAll`, `findByte`, `findHeadEnd`, `clear`); HTTP framing (`Http.parse`); the JSON of bodies
  and tokens (`Json.parseTop`, `Json.decodeString`); HMAC-SHA256 (`Hmac.mac`, `Feed/Sha256.lean`)
  and base64url (`Jwt.b64Encode`, `Jwt.b64Decode`); in `Feed/Handler.lean`, routing (`route`), one
  post object (`postJsonAfter`), the feed's body (`feedFromPage`, `Page.authored`) and the
  responses of the feed and of a post, written in place after their heads (`respondFeed`,
  `respondPost`); and the idle policy (`Idle.pick`, `Feed/Server.lean`). No proof unfolds the
  definitions of `Http.parse`, `findHeadEnd`, `parseTop`, `decodeString`, `Hmac.mac`, `b64Encode`
  and `b64Decode`. Six are not called while serving: outside the self-test, `appendLitEsc`,
  `appendLitNat`, `appendSlice`, `findHeadEnd` and `postJsonAfter` are called only from the Lean
  bodies of other leaves, which the binary replaces with C, and `Idle.pick` (`feed_idle_pick`) is
  called only by the self-test, as the transport calls the C `idle_pick` itself. Of the others,
  `feedFromPage` runs only inside the C `respondFeed`, for a page with a byte to escape or an id or
  like count of 2⁶³ or more, and `clear` runs from Lean at most once per process (the compiler
  makes `Bytes.clear ByteArray.empty` a constant). The self-test compares every leaf with its Lean
  definition on 20,000 random inputs. The C of three leaves differs from their Lean definitions on
  numbers of 2⁶³ or more, which the server never passes: `findHeadEnd` with `lo` ≥ 2⁶³, `Idle.pick`
  with numbers ≥ 2⁶³ and `decodeString` with `hi` ≥ 2⁶³; the self-test's inputs for these three
  stay below 2⁶³ (those of other leaves go past 2⁶⁴). The JSON leaves have no fuel (a comment in
  `runtime/leaves.c` argues that the Lean fuel, the text's length plus one, never runs out) and
  keep nesting on an explicit stack; several leaves read up to 32 bytes past an input's end,
  inside its allocation, which `checkHttpRoom` and the inputs with spare capacity exercise; the
  HMAC uses the SHA extensions when the build CPU has them, so the self-test checks whichever path
  was compiled. During development, mistakes planted in the C each failed the self-test, among
  them a flipped bit of the MAC, a wrong base64url character, an off-by-one in the rowid search, a
  413 limit off by one, a surrogate pair read past the string and a 21st post; the shipped
  self-test plants none.
* **SHA-256 and HMAC** (`Feed/Sha256.lean`): not proved equal to FIPS 180-4 or RFC 2104. The
  self-test checks the Lean `Sha256.hash` against five test vectors (NIST's two examples for
  SHA-256 among them), and the C `Hmac.mac`, on keys from the Lean `Hmac.Key.of`, against cases 2
  and 6 of RFC 4231.
* **What the theorems take as given**: what these functions return (the theorems are about what the
  handler does with it): HTTP framing (`Http.parse`), the JSON of bodies and tokens
  (`Json.parseTop`, `Json.decodeString`), base64url and HMAC-SHA256 (`Jwt.b64Encode`,
  `Jwt.b64Decode`, `Hmac.mac`, `Hmac.Key.of`), the number and length checks (`Json.numGt`,
  `Jwt.positiveInt`, `trimRange`, `codePoints`), and with them whether a token or a post body is
  accepted (`authenticate`, `postText`). No theorem covers the `Bearer ` prefix, the checks of
  `Jwt.verify` outside `verify_ok_sound` (exactly two dots, `nbf`, the kinds of `sub` and
  `username`), or a body's trim and 500 code points. `decide_spec` holds for any `Req`, parsed or
  not, `runBatch_spec` uses `Http.parse` as it is, and `verify_ok_sound` is stated in terms of these
  functions. That `ctEq` compares the signature in constant time (RFC 7518 §3.2) is not proved
  either: it is compiled Lean, and its timing is up to the Lean compiler and gcc.
* **HTTP**: the framing follows `Feed/Http.lean`, which is not checked against RFC 9112 or RFC 9110
  and differs from them in several places, among them: any `Transfer-Encoding` is answered 400 and
  the connection closed, no response carries a `Date`, `Expect: 100-continue` gets no
  `100 Continue`, and a method other than `GET` and `POST`, `HEAD` among them, gets 404 with a
  body.
* **Stored text is UTF-8**: `escape` copies every byte but `"`, `\` and the controls, so a response
  is a JSON text (RFC 8259 §8.1) only if the bodies, timestamps and usernames in it are UTF-8. A
  new post's body and a token's username come from `Json.decodeString`, which writes U+FFFD for
  bytes that are not UTF-8; the seed's rows are taken as they are.
* **The transport** (`runtime/uring.c`): moves bytes between sockets and `Feed.batch` and does not
  parse HTTP. What a client receives also depends on what it does around the batch. It sends a
  batch's replies only after `Feed.batch` returns, and `runBatch` returns after the commit. It
  keeps the input a batch did not use and puts it before the connection's next input; input left
  behind a write or after 64 requests comes back only in a batch that may write, and input that did
  not fit in a capped batch (Overload, below) in a later one. It closes, without an answer, a
  connection whose unanswered input exceeds 64 KiB (`MAX_IN`), so a request of more than 64 KiB
  whose `Content-Length` is within `Http.maxBody` (1 MiB) is never answered: `Http.parse` waits for
  its body, and the transport closes first. After the peer's FIN it answers the complete requests
  it has, drops an unfinished one and closes; a failed send closes the connection; a connection
  closed while its input waited for the batch gets no reply. That every complete request is
  answered in the end rests on it too: held input waits for a batch that may write, which comes
  50 ms after the last unless writes are paused (at most 300 ms, `runtime/db.c`). Its choice of
  idle connections to close is `idle_pick`, the C version of `Feed.Idle.pick`. The WAL shim
  (`runtime/walvfs.c`) only merges the write calls of one commit; the page cache wrapper
  (`runtime/db.c`) only skips a scan that would find nothing.
* SQLite itself, Lean's compiler, runtime and kernel, gcc, Linux.

## Design

The numbers in this section come from A/B runs made while each change was developed, on the build
of that time; each says where it was taken (the setups are under Measurements).

**Batches.** Each event-loop iteration hands the new input of every connection to Lean in one call
(`Feed.batch`). The batch is one SQLite transaction: all reads use one snapshot, through B-tree
cursors opened once per batch and the batch's pages (Reads, below); the writes follow in
connection order as prepared statements; then one commit. Answers are sent after the commit, so a
201 is always committed (WAL, `synchronous=NORMAL`: a commit survives a crash of the process, and a
power loss or a crash of the machine can roll back the most recent ones). While busy, after an
iteration that finished early the loop waits until ring 0 has 64 completions or 2 ms have passed
since that iteration began, so a batch collects everything that arrived meanwhile (many requests
then share one wakeup, one transaction, one Lean call and one burst of sends); under saturation
iterations run back to back.

**Writes in fewer batches.** A batch may write at most every 50 ms (`writesOk` is false in the
batches between); a write waits for that batch, reads do not. A commit then carries the writes of
many periods, and the pages they share (the end of `likes`, the start of the feed index, the
recent part of `likes_post_id_idx`) go to the log once per commit instead of once per period. On
the cloud VM at 350,000 users, a 10 ms window wrote about 7,400 log frames a second (30 MB/s, and
the volume's 3,000 writes per second were in use once checkpoints copied them); 50 ms, with
checkpoints every 2,000 frames, used 9 % less CPU in the same hold, and disk writes fell from about
2,900 to 2,100 a second. A write waits up to 50 ms for its batch while writes are not paused
(Writes, below); in that hold likes took p50 30 ms and p99 106 ms, and reads p99 9 ms.

**Overload.** When more input arrives than an iteration can serve in about 6 ms, the batch is
capped: the input that arrived since the last iteration goes first and connections left over
from earlier iterations (a backlog, oldest first) fill the remaining room. The cap follows a
running estimate of the time per connection, so the first batch after a burst is capped too.
New requests are then still answered within an iteration or two, before the kernel's
delayed-ACK timer (40 ms) adds a packet to each of them: a server whose responses take longer
sends two packets per request instead of one. Without a backlog, batches stay below the cap.

**No caching across requests.** Every request reads its rows from SQLite while it is served (the
feed reads the index, the rows, each author and the like counts); every token's signature is
computed.

**Reads.** Over the batch's pages: a page that a read fetches stays referenced until the batch's
reads end, as an open cursor's pages do, so a later read of the batch finds it without the pager;
every read still searches the pages for its own rows. The feed reads the first entries of
`posts_created_at_id_idx` in order, a loop over each page's cells. When their ids are consecutive
(new posts get increasing ids), one descent of `posts` finds the first row and each next row is
the cell before the previous one's, its rowid checked (a leaf boundary or a gap: another descent);
ids that lie close together get one descent per leaf they fall in, spread-out ones the grouped
search (Credits), and the rows of a page of more than 64 posts are read through SQLite's cursor.
The like counts come from one range scan of `likes_post_id_idx` when the ids lie close together,
and each author from a descent of `users` of its own: on an interior page the search starts where
a line through the page's first and last keys puts the key (the line is kept for the next key of
the read that meets the page), and the leaf is entered at the cell where the key would be if its
rowids were dense from the bound the parent's cells give (in `users` they are). Records are read
where their one-byte header varints put the columns. Pages come through SQLite's pager, page cache
and WAL; only the bytecode engine is skipped.

**Writes.** `INSERT OR IGNORE` with `created_at` bound in the schema default's format: for these
values the only constraints that can fail are the like's primary key, which it ignores as
`ON CONFLICT DO NOTHING` would, and the body's `CHECK` for a body that starts with U+0000 (above),
whose row it skips; so no statement can abort halfway and SQLite keeps no statement journal. A new
post's id comes from `sqlite3_last_insert_rowid` instead of `RETURNING`. Checkpoints run on a
second thread with its own connection every 2,000 frames (passive, so the request thread never
waits on an fsync). SQLite restarts the WAL only when a writer finds it fully checkpointed, which
never happens under constant writes; past 8,192 frames, writes pause (reads go on) until a
checkpoint has copied what is left, at most 300 ms (a few milliseconds on a fast disk; more where
writes per second are limited, as on the cloud VM's volume). A VFS shim (`runtime/walvfs.c`)
issues each commit's WAL frames in writes of up to 64 KiB (one write for a commit of up to 15
frames) instead of two per page.

**Page cache.** SQLite's own (`SQLITE_CONFIG_PCACHE2`), wrapped: at commit SQLite drops cached
pages past the end of the file, and the default cache scans its whole hash table for them once
any key at or past that point was seen. `balance_nonroot` parks a page under the lock-byte page's
number for a moment when it reorders new pages, so after every page split the next commit scanned
the whole cache (16,000 pages in the 64 MB cache this entry used when it was measured). The
wrapper tracks the largest key a page of the cache holds and passes the truncation on only when it
can find something (2.5 % of the handler's CPU in the in-process benchmark). The caches take their
first 2,880 pages from one pool of 12 MB (`SQLITE_CONFIG_PAGECACHE`) in huge pages, faulted in at
startup before the process turns them off, and the rest of the request thread's 48 MB from malloc:
in 4 KB pages the pool would need more TLB entries than there are, so reads of random pages (a
feed's authors, a post's leaves) would walk the page tables (the pool saved 3 % of the handler's
CPU in the in-process benchmark, with the 8 MB cache of the time).

**Compiling.** gcc builds both the C that Lean emits and the C runtime with `-O3 -march=native`,
profile-guided: `build.sh` builds an instrumented binary, runs it on a made-up database and
workload (`runtime/train.c`: its own users and posts, no seed data; the user model of
`bench/load.js` and some rejected requests), then builds the final binary with the profile
(9 % less CPU for the handler than Lean's clang at `-O3`, in the in-process benchmark). If any
step fails it builds without the profile. `-march=native` also decides the SHA-256 compression: it
uses the SHA extensions when the build CPU has them and scalar code otherwise (the box's Xeon
Platinum 8260 has none).

**Transport.** Several io_uring rings, each with a table of direct socket descriptors (a ring's
table is limited by `RLIMIT_NOFILE`, so the rings together hold more than 65,535 connections);
eight `SO_REUSEPORT` listeners; multishot accept; `DEFER_TASKRUN` on every ring. Rings are made as
connections fill them (ring 0 at startup, each later one 8,192 connections before the one before
it is full), so only as many are busy as the connection count needs. The loop enters each ring
once per iteration, in #264's order: each other ring submits what the last batch queued and runs
its task work, then ring 0 submits and waits, in a timed `io_uring_enter`, for 64 completions or
the rest of its 2 ms period; input that reaches another ring during that wait is read in the next
iteration. On the local harness at 280,000 users (`FEED_STATS`, with the multishot receive below)
that made 0.079 calls of `io_uring_enter` per request against 0.207 for the previous design, and
about 540 wakeups of the loop a second against 480. With `FEED_RINGS_PLAIN` the other rings run
their task work as it comes, as in #245, so their completions are read from shared memory without
entering them; that also made 0.079 calls per request, and each of those rings' completions then
woke the loop out of ring 0's wait, about 4,200 wakeups a second. `FEED_RINGS_UPFRONT`, and the
idle policy below (whose race-free close was tested only with that design), keep the previous
design: every ring made at startup with a listener of its own, entered twice per iteration, and a
sleep between iterations.

Each connection has one multishot receive into its ring's 512 provided buffers of 1 KB, the design
of rust-iouring-kensac #264: a request costs one completion. A burst that takes every buffer of a
ring ends the receive (`ENOBUFS`); its connection joins the ring's read queue, and after the
iteration's completions are in, the loop issues one non-blocking read per queued connection
(never more on a ring than its buffers), in up to eight rounds while buffers come back, and the
reads complete during the submit. A read that fills its buffer queues the connection again, so a
burst of any size waits in the queue; once a read shows the socket drained, the receive is armed
again. Each round enters the rings with queued connections once more; on the local harness at
280,000 users no receive ended in a whole run. The receive holds its own reference to the socket:
Linux up to 6.12 frees a direct descriptor that was closed only once every request issued on its
ring before the close has completed, and a request on a direct descriptor that stays armed is such
a request for as long as it lasts, so one idle connection kept every socket closed after it (in a
local test with polls on direct descriptors, after 2,000 peers closed their connections the
server's 2,000 sockets stayed in CLOSE-WAIT until the idle connection closed, and at SIGTERM the
process did not exit until killed). So the receive is armed on a plain descriptor that
`IORING_OP_FIXED_FD_INSTALL` (6.8) makes for the slot and that is closed again at once; a receive
there takes no resource node, and provided buffers are not a resource. On kernels before 6.8,
which lack that operation, each connection waits on a poll of its direct descriptor instead, and
one idle connection again keeps every socket closed after it. Reads and sends, which complete at
once, use the direct descriptor. A close cancels the receive first, linked to the close.

With `FEED_MSRECV=0`, and while the idle policy is configured (its race-free close needs a poll to
remove), each connection waits on a multishot poll instead, armed the same way: a readiness report
puts it on the read queue, so every request costs a report, a submission and a read's completion.
The receive saved no CPU beyond the noise: on the local harness (Linux 6.12) 12.43 µs of the core
per request against 12.63 with polls (three and two runs; the server's cgroup alone 11.46 against
11.10), and on the cloud VM (Linux 6.8) at 340,000 users 60.0 % of the CPU against 58.6 %. It
costs memory: an armed receive takes its async poll entry (96 B) beside the 256 B request, and on
6.12 also a 512 B message header; on the cloud VM the slab came to 4.21 to 4.22 KB per connection
against 4.12 to 4.14 KB with polls (90 to 105 B more), and in a local test on 6.12 with 100,000
idle connections 605 B more, none of it charged to the server's memory cgroup. The receive is the
default because of the tail latency with polls: on the KVM guest with its vCPU's sibling
hyperthread at full duty, 2-minute probes at 320,000 users held p99 58 ms at 81 % of the CPU with
the receive, and with polls p95 46 ms but p99 1.9 s at 79 %, where 1 to 2 % of every kind of
request took 1.5 to 3.7 s.

Kernels newer than 6.8 charge ring memory to `RLIMIT_MEMLOCK` (8 MB under systemd); there a ring
that does not fit is made smaller, or the server runs on fewer rings. Input a batch did not consume
waits in a heap buffer only until the next batch; kept for the connection's lifetime (one for every
connection that had ever waited, which includes every writer outside a write window), these
buffers grew the heap by 117 MB during a 2-minute hold at 360,000 users on the local harness. At
SIGTERM every listener closes first, and the signal handler also wakes the loop's idle wait (in a
local test with 2,000 idle connections the process exited after 0.010 s, against 0.51 s before).

**Memory.** The kernel keeps about 4 KB per open connection (TCP socket, inode, file, the armed
receive or poll), most of it outside the server's cgroup on 6.8 (in a local test on 6.12, about
3.9 KB per connection was charged to it), so on a 2 GB machine the connections can set the limit
before the CPU does. On the cloud VM (1,903 MB) they did for PR #271's first version: near 350,000
connections the page cache that was left was too small for the database's working set, and
requests started waiting on disk reads. The box found that version CPU-bound at 290,000 users, and
on the KVM guest (1,968 MB) this version had 399 MB available at 360,000. The server keeps little
memory of its own, except where a miss would reach the disk: SQLite's cache is 48 MB, enough for
the writes' working set (the likes primary-key index is 29.5 MB in the seed, and each like reads a
random leaf of it). With 8 MB, on the cloud VM at 350,000 users, the end of the ramp evicted the
page cache and the loop read about 1,000 pages a second from disk for 40 s (p99 6 s); with 48 MB it
read about 10 a second and the hold passed with p99 14 ms. The rest is 512 receive buffers of 1 KB
per ring, completion queues larger than a ring's connections (an overflow would allocate per
completion when memory is shortest), and the first 8 MB of the code locked in memory as it first
runs (reclaim would otherwise take the program's own pages back and the loop would stall reading
them from disk). Checkpoints run at nice 19, so on one vCPU they take the CPU only while the loop
waits between batches. Ubuntu's 6.8 kernels from 6.8.0-136 on (and the cloud VM's 6.8 kernel)
reject the registration of provided-buffer rings with a zero reserved field (Launchpad #2162843);
the server retries with a nonzero one, which those kernels accept and a correct kernel never sees.

**Idle connections under a budget.** A safety valve for memory pressure, off by default: it runs
only with `FEED_CONN_BUDGET` (above that many open connections, close the longest idle) or
`FEED_IDLE_MS` (close every connection idle that long) set, and `start.sh` sets neither; with both
unset the transport makes one test per iteration and one per accept or answer, and nothing else. A
forced close costs about 42 µs of a core when its user comes back (on the local harness at 280,000
users with a budget of 270,000: 4,450 closes a second, +6.3 µs of the core per request), so a
budget pays only where the alternative is to run out of memory (on the cloud VM, 350,000
connections took 1.48 GB of slab and 370,000 ran out of memory). With the policy on, connections
wait on polls (`FEED_MSRECV` is ignored) and the transport uses the previous ring design. From 7/8
of the budget on (always, with a timeout), each ring keeps its open connections in order of their
last activity, built once by sorting on the time each accept or answer noted, and drops the lists
again below 3/4. Once per iteration, while more connections are open than the budget or a timeout
is set, the transport merges the heads of those lists, longest idle first, up to
2·`FEED_IDLE_PER_ITER` + 64 candidates (576 by default), flags each one busy or not (`conn_busy`:
input not yet answered, an answer being sent or queued, part of the batch, no armed poll, closing;
or its ring has completions not yet handled or no room to submit), and calls the C `idle_pick`,
whose Lean definition the theorems above are about, to choose which to close. A close removes the
poll first, linked to the close, so input that reached the socket before the removal cancels the
close and is answered. Input that arrives between the removal and the close is lost with the
connection: in a local test with requests 990 to 1,010 ms apart around a 1,000 ms timeout, 10 of
30,000 requests got EOF or a reset, and the client sent them again.

## Measurements

Each number says where it was taken: the challenge's box, a KVM guest set up like the box, a cloud
VM set up like the box, the local harness, or the in-process benchmark. Only the box's numbers are
box results.

### On the challenge's box

The challenge measured PR #271's first version (30d259c), whose code this version builds on, on
2 November 2026: it held 290,000 users for 5 minutes (27,560 requests a second, p95 30.7 ms, p99
300 ms, no errors) at 82 % of the CPU and 27 µs of CPU per request (peak memory 1,107 MB), and
300,000 failed the 2-minute probe on p99 (2.1 s). rust-iouring-kensac #264, first at 310,000, used
25 µs per request at 82 % of the CPU there (p99 190 ms). This version has not been measured on the
box.

### On a KVM guest set up like the box

These are the guest's numbers, except those marked as the box's. The guest has 1 vCPU and MemTotal
1,968 MB (the box: 1,967 MB) and runs Ubuntu 24.04 with 6.8.0-117-generic (the box's kernel), with
virtio disk and network, under QEMU with `-cpu host` on a Cascade Lake Xeon host (the box's CPU
generation), its vCPU pinned to one hyperthread at 3.1 GHz. The vCPU's sibling hyperthread runs a
CPU-bound load (stress-ng `matrixprod`) at 65 % duty, chosen to bring #264 at 310,000 users near
the box's 82 % of the CPU (78.1 % in its 5-minute hold below; at 0 % duty it used 56 %, at 25 %
63 %, at 50 % 71 %; at 100 % it saturated). Load: k6 1.3.0 with `bench/load.js` from five load
generators of 16 vCPUs, four k6 processes each (one per server address); 60 s ramp, 5-minute hold,
30 s ramp-down; a fresh copy of the seed for every run. Pass is the leaderboard's thresholds over
the whole run (p95 < 500 ms, p99 < 1 s, under 1 % errors); CPU is the server's cgroup over the
hold, as in the leaderboard's CPU % column. The runs of this version used the build from before the
receive became the default, with `FEED_MSRECV=1`: the code path this version takes by default.

| Entry (box score) | 5-minute holds on the guest (CPU, p99) | Guest's score |
|---|---|---|
| rust-iouring-kensac #264 (310,000) | 310,000 pass (78.1 %, 77 ms); 320,000 pass (80.8 %, 88 ms); 330,000 fail (p99 1.58 s) | 320,000 |
| mojo-iouring-oh-meh #257 (304,000) | 300,000 pass (76.7 %, 55 ms); 310,000 pass (78.8 %, 602 ms) | 310,000 or more |
| c-iouring-pavlyhalim #245 (295,000) | 290,000 fail (p99 1,006 ms); 300,000 pass (74.3 %, 912 ms) | 300,000 (290,000 failed) |
| PR #271's first version, 30d259c (290,000) | 290,000 pass (70.1 %, 20 ms); 300,000 pass (72.9 %, 26 ms) | 300,000 or more |
| this version (not measured on the box) | 330,000 pass (70.1 %, 163 ms); 340,000 pass (72.8 %, 55 ms); 350,000 pass (75.0 %, 90 ms); 360,000 pass (77.8 %, 125 ms); 370,000 pass (79.7 %, 148 ms, 0.05 % errors) | 370,000 or more |

The guest scored #264 and #245 2 to 3 % above their box scores and #257 at least 2 % above
(320,000 was not run), and the previous version of this entry at least one step above its box
score: 300,000 passed there and failed on the box. It also favored that version against #264: on
the guest it used 4 % less CPU per user than #264 (70.1 % at 290,000 against 78.1 % at 310,000), on
the box 8 % more per request (27 against 25 µs). This version used 15 % less CPU per user than #264
on the guest, and at 360,000 users 399 MB of memory was still available. The guest may overstate
this version as it overstated the previous one; the box will decide.

Earlier stand-ins were further off. A 1-vCPU slice of a larger cloud VM with a CPU-bound load on
its sibling hyperthread held the previous version at 330,000 (box: 290,000) and #264 at
310,000 (its box score); with 18 % of its CPU taken away at fine grain it reproduced the previous
version's box result (290,000 passed with p99 307 ms, 300,000 failed) but failed #264 at 310,000,
and there this version held 330,000 and failed 340,000. The cloud VM below held the previous
version at 340,000 or 350,000 and #245 at 333,000 (box: 290,000 and 295,000). Box runs vary as
well: the same code of ada-native-visseranalytics held 270,000 in one run and failed 260,000 in
another, and c-iouring-pavlyhalim's #278, a `db.h` tuning of #245 (other code), held 290,000 at
28 µs per request where #245 had held 295,000 at 26.

### The setups that Design names

* **The cloud VM**: booted with `nr_cpus=1 mem=2048M` (one Xeon vCPU, 1,903 MB, no swap),
  Ubuntu 24.04 with a 6.8 kernel, a network volume limited to 3,000 writes per second, systemd and
  load as above. Its vCPU is faster than the box's: #245 used 15 to 16 µs per request there and
  26 µs on the box.
* **The local harness**: the server in a container pinned to one core of a Sapphire Rapids Xeon
  with a 1.75 GB memory limit, its packets handled on the same core; a C load generator follows
  `bench/load.js` (feed, think 3 to 7 s, post, think 3 to 8 s, 15 % like, 2 % new post, idle 5 to
  15 s) from 32 source addresses, with a 60 s ramp, a 120 s hold and a 30 s ramp-down. Its µs per
  request are of the core at full speed.
* **The in-process benchmark**: the handler and the C runtime driven by synthetic batches of the
  load mix, without the network.

## Credits

### What is ours

The design and the code of this entry are ours, except what the next subsection names.

* **The Lean request path.** The handler as compiled Lean on the request path (batch, request
  loop, dispatch, token checks, body rules, the feed's reads, the writes, the responses), the
  `Store` class with its two instances (C over SQLite for the server, a state monad over the
  specification's tables for the proofs), and `Feed/Spec.lean`'s statement of the API down to the
  response bytes (its tables and reads follow #153's `Model.lean`).
* **The theorems.** The 124 theorems in `Feed/Proofs/` about those definitions: both reads'
  complete responses byte for byte, the writes, accepted tokens, routing, one request
  (`decide_spec`) and the batch as a serial execution (`runBatch_spec`), apart from the lemma the
  #153 bullet names.
* **C leaves checked against Lean.** Each C leaf is `@[extern]` on its Lean definition, and the
  self-test compares the C with those definitions on random inputs and the storage operations with
  the model on a random database, so the build fails if they differ (testing the grouped search
  against `sqlite3BtreeTableMoveto` is #233's idea).
* **The idle policy** as a Lean definition with 10 theorems (prior art for its two settings is
  below).
* **In the C runtime.** Polls and receives armed on plain descriptors made by
  `IORING_OP_FIXED_FD_INSTALL`, after we found that Linux up to 6.12 frees a closed direct
  descriptor only once every older request on its ring has completed, so one long-lived poll on a
  direct descriptor kept every later-closed socket; the WAL shim (#158 and #233 have similar ones);
  the page cache wrapper's code; the pool of huge pages for SQLite's cache; the batch's page table;
  the responses written in place after their heads; the overload cap and its delayed-ACK reasoning;
  the write window's size; `INSERT OR IGNORE` without a statement journal; the WAL pause's 300 ms
  limit; the 2 ms iteration period (#233 and #245 also wait up to 2 ms while busy); `train.c`.
* **The measurement setup** in Measurements: a KVM guest set up like the box and calibrated
  against three other entries' box scores, and the finding there that readiness polls left 1 to
  2 % of requests at 1.5 to 3.7 s, which made the receive the default.

### From other entries

Where an entry's code is not named below, the idea came from its README and the code here is ours.

* **lean-derive-hargup #153**, the first Lean entry, whose runtime and model this one started from.
  Code: `install.sh`; `build.sh`'s SQLite download and first eight `-DSQLITE_` flags; `server.c` as
  one translation unit with the amalgamation; in `db.c` the SQLite helpers, the record decoder,
  `cur_payload`, `likes_seek_ge` (c-iouring-pavlyhalim's `src/btree.h` has the same three; ours
  were in our `db.c` before we read it), the layout check, `db_open`'s order, the batch's `BEGIN`
  plus `sqlite3BtreeBeginTrans` and the checkpoint thread; the skeleton of `uring.c` (ring setup,
  `user_data` tags, submissions and completions, the listener, ring sizes from `RLIMIT_NOFILE`, the
  held-connection list). Designs: `Feed/Json.lean`'s validating scanner that records the spans of a
  few wanted top-level keys, the last duplicate winning (its `runtime/json.c`), and
  `Feed/Http.lean`'s limits and refusals (a 16 KiB head, 400 for a conflicting `Content-Length` or
  any `Transfer-Encoding`, 413, 431; its `runtime/http.c`), which the JSON and HTTP leaves now
  implement in C; `Feed/Jwt.lean`'s three results and the order of its checks (its
  `runtime/jwt.c`); `readLoop`'s rule that a connection's requests after a write wait for the next
  batch (its `runtime/uring.c` `conn_hold`), decided here in Lean. Ideas: B-tree cursor reads with
  SQL writes; a batch as one transaction, reads first, answered after its commit; waiting after a
  light iteration so that the next one serves a batch; a `SO_REUSEPORT` listener per ring (the
  design `FEED_RINGS_UPFRONT` keeps); the words "that is the key schedule" for the HMAC key blocks
  computed once (its README, JWT). Lean: a model of the API over lists of rows with proofs checked
  in `build.sh`; `Feed/Spec.lean`'s tables and reads (`newerEq`, `feed`, `view`, `getPost`,
  `nextPostId`, the like insert) follow its `Model.lean`; the proof of `filterMap_congr_mem`
  follows its `filterMap_congr'`. The summary table and parts of Design paraphrase its README.
* **c-iouring-pavlyhalim #233 and #245.** Code: the grouped rowid search (`lookup_many`,
  `page_key`, `page_lower_bound`, `hit_record`) is adapted from `bt_seek_many`, `bt_cell_key`,
  `bt_lower_bound` and `bt_hit_record` (`src/btree.h`, #233, kept in #245). The guess inside a page
  from `bt_lower_bound` (start where a line through the page's first and last keys puts the key,
  try its neighbour, then bisect), which `page_lower_bound` took over, is also used by
  `lookup_sorted`, by `find_rowid` on each level and by `it_lower` in the pages of
  `likes_post_id_idx`; keeping every page a batch's reads fetch referenced until its reads end, the
  direct hit at `k - k0` and computing the guess in integers are ours. `accept_policy` in
  `runtime/uring.c` is adapted from its `accept_policy` (`src/server.c`, #245): rings are made as
  connections fill them (ring 0 at startup, each later one 8,192 connections before the previous
  one is full), and the multishot accepts of eight `SO_REUSEPORT` listeners are armed on the ring
  being filled and cancelled 4,096 slots before a ring is full. `sys_enter_wait`, the timed
  `io_uring_enter` on ring 0, follows its `ring_wait` (`src/sys.h`) and waits for 64 completions as
  its loop does; `FEED_RINGS_PLAIN` runs its other ring design, with ordinary task work on the rings
  after ring 0, for measurements. The buffer-ring retry is from #245's `src/sys.h`
  (fsharp-kestrel-delneg #201 and ada-native-visseranalytics #211 had the same retry earlier).
  Ideas from #233: testing that search against `sqlite3BtreeTableMoveto`, the page cache wrapper,
  the training database's sizes and request mix. From #245's README (Connections, At the memory
  wall, SQLite): reads through a read queue, one non-blocking read per queued connection and never
  more than the ring's buffers (the burst path of the receive, and with polls every read), polls for
  readiness, removing the poll before a close, 65,536-entry completion queues, smaller rings on
  `ENOMEM`, keeping the server's own memory small at the memory wall (the 48 MB cache here came from
  our runs on the cloud VM), checkpoints at nice 19, locking the code (c-iouring-public #158 had it
  first), its 90 s keep-alive close, and its memory valve, off by default because it did not pay
  (the idle policy here is off by default too).
* **rust-iouring-kensac #264** (its README, Connections, and the receive, read and pass-loop code
  of its `src/main.rs`). The receive path, the default here: one multishot receive per connection
  into the ring's provided buffers, and after a burst (`ENOBUFS`) the connection is read through the
  read queue until a read shows the socket drained, then the receive is armed again. The order in
  which the loop enters the rings: every other ring submits and runs its deferred task work without
  waiting, then ring 0 submits and waits in a timed `io_uring_enter`, where the previous loop
  entered every ring twice and slept between iterations. It also makes rings as they fill. The C is
  ours, its completion handling following that of `src/main.rs`; arming the receive on the
  `FIXED_FD_INSTALL` descriptor is ours, and a close cancels the receive by its `user_data`,
  hard-linked to the close, as #245's close cancels its poll.
* **objc-uring-delneg #154.** Inserts without `RETURNING`: `created_at` formatted and bound by the
  server, the id from `sqlite3_last_insert_rowid` (#233 too); a profile-guided build trained by the
  server's own code on made-up data, with a plain build as fallback (#233 and #228 also use PGO);
  the cause of the page cache's scan, `balance_nonroot` parking a page under the lock-byte page's
  number.
* **mojo-iouring-oh-meh #224.** A read on the checkpoint connection at open (its README: passive
  checkpoints do nothing without one); the `PROVIDE_BUFFERS` fallback (buffers returned in runs,
  completions skipped); the phrases "cursors opened once per batch" and "only the bytecode engine is
  skipped" from its README and "never waits on an fsync" from its `src/shim.c`.
  crystal-epoll-faustinoaq #193 had the checkpoint read and c-iouring-raoz #26 the
  `PROVIDE_BUFFERS` runs earlier.
* **c-iouring-moneebullah25 #228** (first in #156): pausing writes, reads going on, past 8,192 log
  frames until the log can restart; #153's and #224's READMEs explain why group commit needs it.
  Its README also protects memory by closing keep-alives idle for 60 s and by shedding connections
  near a connection cap, the two settings of the idle policy here (`FEED_IDLE_MS`,
  `FEED_CONN_BUDGET`); the policy's choice, its Lean definition, its theorems and its C are ours.
* **v-epoll-i098 #191.** The feed plan: the newest ids from `posts_created_at_id_idx`, one scan of
  `posts` when they are consecutive, one range scan of `likes_post_id_idx` (#153's `feed_tail` and
  `range_counts` also walk `posts` backward and count likes by one range scan; c-iouring-raoz #26
  and cpp-eastl-io_uring-pgo-hendy643 #32 had the plan earlier). #191 also holds new writes for
  5 ms after a commit so that they share the next one, the same idea as the write window here.
* **c-iouring-public #158**, via #245's README: the measurement behind nice 19 (#158's README: at
  the memory wall a checkpoint pass of 300 to 430 ms at nice 0 took half the vCPU from the loop; 2
  of 3 holds at the edge died at nice 0, 3 of 3 held at nice 19).

Related, and not a source as far as we know: c-epoll-TheFirstIstari #64 closes its oldest idle
keep-alive connections under file-descriptor pressure, as the idle policy here does under a
budget, and c-iouring-public #158's memory floors, off by default, close keep-alives idle for 20 s
or more when less than 120 MB is available.

## Build and run

* `install.sh` (root, once): build-essential, curl, ca-certificates, unzip, zstd; Lean
  `nightly-2026-10-30` from the official nightly release, checked against its SHA-256.
* `build.sh` (user): downloads the pinned SQLite amalgamation (SHA-256 checked); `lake build`, which
  checks every proof and compiles `Feed/` to C; gcc builds that C with the C runtime, an
  instrumented binary first, its training run, then the profile-guided `bin/server`; runs the
  self-test.
* `start.sh`: `bin/server` in the foreground, configured by `SQLITE_PATH`, `JWT_SECRET`, `HOST`,
  `PORT`; it sets `UV_USE_IO_URING=0`, so the Lean runtime's libuv opens no io_uring of its own.
  The `FEED_*` environment variables override defaults for measurements or turn on the idle
  policy; `start.sh` sets none of them.
