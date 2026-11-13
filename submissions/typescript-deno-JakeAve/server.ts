// Raw TCP (poller.rs on Linux, Deno.listen on macOS) + a minimal HTTP/1.1 parser + SQLite built by build.sh, over
// FFI. One process: a write connection, a read connection for while a COMMIT is in flight, and render.c's commit
// and checkpoint threads. SQLite renders the JSON. Deno.serve costs ~19 KB per idle connection; this costs ~7.
import {
  ckptStart,
  ckptStats,
  ckptWaiting,
  Db,
  type Stmt,
  wrCommit,
  wrDone,
  wrStart,
  wrStats,
} from "./sqlite.ts";
import { Buffer } from "node:buffer";
import { createHmac, timingSafeEqual } from "node:crypto";

const SQLITE_PATH = Deno.env.get("SQLITE_PATH");
const JWT_SECRET = Deno.env.get("JWT_SECRET");
if (!SQLITE_PATH || !JWT_SECRET) {
  throw new Error("SQLITE_PATH and JWT_SECRET are required");
}
const SECRET: string = JWT_SECRET; // the narrowing above does not reach authenticate()
const HOST = Deno.env.get("HOST") || "127.0.0.1";
const PORT = Number(Deno.env.get("PORT") || 3000);

// Durability: FULL fsyncs the WAL on every COMMIT; NORMAL (1) would not. Every connection uses it.
const SYNC = 2;
const PRAGMAS = `PRAGMA journal_mode = WAL; PRAGMA synchronous = ${SYNC};
  PRAGMA cache_size = -65536; PRAGMA mmap_size = 1073741824; PRAGMA journal_size_limit = 67108864;
  PRAGMA temp_store = MEMORY; PRAGMA foreign_keys = OFF; PRAGMA wal_autocheckpoint = 0;`;
// Two connections on the request thread. db writes, and reads too while it is free: its own commits never
// reset its page cache. A COMMIT's fsync (2-3 s when DO's disk stalls) runs on render.c's commit thread while
// db is its; rdb answers reads meanwhile.
const db = new Db(SQLITE_PATH);
db.exec(PRAGMAS);
// Inline path: a write that meets a TRUNCATE checkpoint holding the write lock waits it out. The poller sets
// this back to 0: there the write queues (see write()) and the request thread never sleeps.
db.exec("PRAGMA busy_timeout = 1000");
const rdb = new Db(SQLITE_PATH);
rdb.exec(PRAGMAS);
// Checkpoints run on render.c's thread, on its own connection (a PASSIVE one every 100 ms), so the request
// thread never pays for one. Not EXCLUSIVE locking: that connection needs the shared-memory WAL index.
ckptStart(SQLITE_PATH, 100, SYNC);
// One transaction held open on each connection (PR #19's trick): autocommit pays transaction setup per
// statement (an fstat, page-cache validation). db's writes commit before their response (rule 6), via
// commit(); synchronous = FULL, so the COMMIT fsyncs the WAL and a power cut loses nothing that was answered.
// rdb's read transaction is ended and reopened (refreshRead) at most every 20 ms after a COMMIT lands: SQLite
// drops a connection's whole page cache when it reopens on a WAL another connection changed (the droplet paid
// +37 µs/req doing that per commit), and a read served from rdb may trail a commit by that much.
// The poller: one COMMIT per px_wait batch, on the commit thread (group commit, fsync off this thread); the
// batch's write responses wait for it. macOS (dev, Deno.listen): commit() runs inline.
const beginStmt = db.prepare("BEGIN");
const commitStmt = db.prepare("COMMIT");
const rBegin = rdb.prepare("BEGIN"), rCommit = rdb.prepare("COMMIT");
const run = (s: Stmt) => {
  s.step();
  s.done();
};
run(beginStmt);
run(rBegin);
let rStale = false, lastRefresh = 0; // a COMMIT landed since rdb's snapshot; when it was taken
// How long rdb's snapshot may trail a landed COMMIT (ms). Each refresh drops its page cache; 100 and 500
// cost the same under slow commits, and 500 stalls a TRUNCATE's reader wait (bench/results.md).
const REFRESH_MS = 20;
function refreshRead(): void {
  run(rCommit);
  run(rBegin);
  rStale = false;
  lastRefresh = performance.now();
}
// DIAG counters for the current 10 s window (the poller fills commits and worstCommitMs from wrStats).
let writes = 0, commits = 0, worstCommitMs = 0;
function timedCommit(): void {
  const t = performance.now();
  run(commitStmt);
  const ms = performance.now() - t;
  if (ms > worstCommitMs) worstCommitMs = ms;
  commits++;
}
let deferCommit = false, pending = false; // poller: a write in this batch is not committed yet
// Poller: db is not ours right now (a COMMIT is in flight on the commit thread, or a TRUNCATE checkpoint holds
// the write lock), so a write's statements wait: its handler returns a thunk that runs them once db is back
// (servePoller), and reads go to rdb. Never set on the inline path.
let wBusy = false;
// A write's statements now, or the thunk. SQLITE_BUSY on the first statement: the checkpointer holds the
// write lock, or it truncated the WAL under db's read snapshot (a write from a stale snapshot is refused
// outright, no busy wait). Nothing changed yet; with nothing uncommitted on db, start its transaction over so
// the retry sees the current WAL. Inline: retry now (busy_timeout waits out the lock). Poller: next batch.
function busy(e: unknown): boolean {
  if ((e as Error).message !== "database is locked") return false;
  if (!pending) {
    run(commitStmt);
    run(beginStmt);
  }
  return true;
}
function write(exec: () => Uint8Array): Resp {
  if (wBusy) return exec;
  try {
    return exec();
  } catch (e) {
    if (!busy(e)) throw e;
    if (!deferCommit) return exec();
    wBusy = true;
    return exec;
  }
}
export function commit(): void {
  writes++;
  if (deferCommit) {
    pending = true;
    return;
  }
  timedCommit();
  run(beginStmt);
  refreshRead();
}
let lastDiag = performance.now(), lastRuns = 0;
function diag(extra = ""): void {
  const now = performance.now(), s = (now - lastDiag) / 1000;
  const { runs, worstMs, log, ckpt } = ckptStats(true);
  console.error(
    `DIAG writes/s ${(writes / s).toFixed(0)} commits/s ${
      (commits / s).toFixed(0)
    } worst commit ${worstCommitMs.toFixed(1)} ms · checkpoints ${
      runs - lastRuns
    } (worst ${worstMs} ms) · WAL log ${log} ckpt ${ckpt}${extra}`,
  );
  lastDiag = now;
  lastRuns = runs;
  writes = commits = worstCommitMs = 0;
}

// Rows for render.c's render_posts, which writes their JSON in C in one FFI call, so the bytes never become
// a JS string. Same output as json_object + group_concat in SQL, which was 41% of the feed query's time.
const POST_ROW =
  `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
     FROM posts p JOIN users u ON u.id = p.user_id`;
// Reads: on db while it is free (warm cache, sees everything), on rdb while a COMMIT is in flight.
const FEED = `${POST_ROW} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`;
const feedW = db.prepare(FEED), feedR = rdb.prepare(FEED);
const POST = `${POST_ROW} WHERE p.id = ?`;
const postW = db.prepare(POST), postR = rdb.prepare(POST);
const createStmt = db.prepare(
  "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
);
// 0 changes: already liked, or no such post.
const likeStmt = db.prepare(
  "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING",
);
const existsStmt = db.prepare("SELECT 1 FROM posts WHERE id = ?");
const healthW = db.prepare("SELECT 1"), healthR = rdb.prepare("SELECT 1");

const enc = new TextEncoder();
const dec = new TextDecoder();
const REASON: Record<number, string> = {
  200: "200 OK",
  201: "201 Created",
  400: "400 Bad Request",
  401: "401 Unauthorized",
  404: "404 Not Found",
  500: "500 Internal Server Error",
  503: "503 Service Unavailable",
};
// Poller: near the fd limit (px_crowded, checked once per batch), this batch's responses close their
// connection, so users past the limit reconnect per request instead of waiting in the listen queue. Which
// users hold a connection rotates; about (users - limit) / users of requests pay a reconnect either way.
// ponytail: closes every responder in a crowded batch, so open connections dip up to a batch below the
// limit; a per-batch budget (px_crowded returning the excess) if that ever shows up.
let crowded = false;
const head = (status: number, n: number) =>
  `HTTP/1.1 ${REASON[status]}\r\ncontent-type: application/json\r\n${
    crowded ? "connection: close\r\n" : ""
  }content-length: ${n}\r\n\r\n`;
// A handler's result: the response, or (poller, db busy) a thunk that runs the write's statements later and
// returns it.
type Resp = Uint8Array | (() => Uint8Array);
// The whole HTTP response in one buffer: one write per request.
function json(status: number, body: string): Uint8Array {
  const b = enc.encode(body);
  const h = head(status, b.length);
  const out = new Uint8Array(h.length + b.length);
  enc.encodeInto(h, out);
  out.set(b, h.length);
  return out;
}
// 200 with s's rows rendered as JSON by render.c (see sqlite.ts render). undefined when single finds no
// row. Worst case: 20 posts × (500 chars × 6 escaped bytes + the rest) fits 128 KB.
const scratch = new Uint8Array(128 * 1024);
function renderResponse(s: Stmt, single: boolean): Uint8Array | undefined {
  const n = s.render(scratch, single);
  if (n === 0) return undefined;
  const h = head(200, n);
  const out = new Uint8Array(h.length + n);
  enc.encodeInto(h, out);
  out.set(scratch.subarray(0, n), h.length);
  return out;
}
const NOT_FOUND = '{"error":"not found"}';
const INVALID_ID = '{"error":"invalid post id"}';
const POST_NOT_FOUND = '{"error":"post not found"}';
const AUTH_ERRORS = [
  "",
  '{"error":"missing bearer token"}',
  '{"error":"invalid or expired token"}',
  '{"error":"invalid token payload"}',
];
const MISSING = 1, INVALID = 2, BAD_PAYLOAD = 3;

// Decimal digits only and positive, otherwise 0.
export function parseId(s: string): number {
  if (s.length === 0 || s.length > 16) return 0;
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const d = s.charCodeAt(i) - 48;
    if (d < 0 || d > 9) return 0;
    n = n * 10 + d;
  }
  return n;
}

type Claims = Record<string, unknown>;
function b64urlJson(segment: string): Claims | null {
  const v: unknown = JSON.parse(
    Buffer.from(segment, "base64url").toString("utf8"),
  );
  return v !== null && typeof v === "object" ? (v as Claims) : null;
}

// Verified on every request: HS256 only, constant-time signature compare, exp checked.
export function authenticate(
  header: string | null,
): { id: number; username: string } | number {
  if (header === null || !header.startsWith("Bearer ")) return MISSING;
  const dot1 = header.indexOf(".", 7);
  const dot2 = header.indexOf(".", dot1 + 1);
  if (dot1 < 0 || dot2 < 0 || header.indexOf(".", dot2 + 1) >= 0) {
    return INVALID;
  }
  let hdr: Claims | null, payload: Claims | null;
  try {
    hdr = b64urlJson(header.slice(7, dot1));
    payload = b64urlJson(header.slice(dot1 + 1, dot2));
  } catch {
    return INVALID;
  }
  if (hdr === null || hdr.alg !== "HS256") return INVALID;
  const sig = createHmac("sha256", SECRET).update(header.slice(7, dot2))
    .digest();
  const given = Buffer.from(header.slice(dot2 + 1), "base64url");
  if (given.length !== sig.length || !timingSafeEqual(sig, given)) {
    return INVALID;
  }
  if (payload === null) return BAD_PAYLOAD;
  if (typeof payload.exp !== "number" || payload.exp <= Date.now() / 1000) {
    return INVALID;
  }
  const { sub, username } = payload;
  if (typeof sub !== "string" || typeof username !== "string") {
    return BAD_PAYLOAD;
  }
  const id = parseId(sub);
  return id > 0 ? { id, username } : BAD_PAYLOAD;
}

function createPost(auth: string | null, raw: Uint8Array): Resp {
  const user = authenticate(auth);
  if (typeof user === "number") return json(401, AUTH_ERRORS[user]);
  let data: unknown;
  try {
    data = JSON.parse(dec.decode(raw));
  } catch {
    return json(400, '{"error":"malformed JSON body"}');
  }
  const body = data !== null && typeof data === "object"
    ? (data as Claims).body
    : undefined;
  if (typeof body !== "string") {
    return json(400, '{"error":"body is required"}');
  }
  const trimmed = body.trim();
  if (trimmed === "") return json(400, '{"error":"body is required"}');
  if (trimmed.length > 500) {
    return json(400, '{"error":"body must be at most 500 characters"}');
  }
  const exec = () => {
    createStmt.int(1, user.id).text(2, trimmed).step();
    const id = createStmt.colInt(0), createdAt = createStmt.colText(1);
    createStmt.done();
    commit();
    return json(
      201,
      `{"post":{"id":${id},"body":${
        JSON.stringify(trimmed)
      },"created_at":"${createdAt}","author":${
        JSON.stringify(user.username)
      },"like_count":0}}`,
    );
  };
  return write(exec);
}

function likePost(auth: string | null, target: string): Resp {
  const user = authenticate(auth); // auth before id (SPEC.md)
  if (typeof user === "number") return json(401, AUTH_ERRORS[user]);
  const id = parseId(target.slice(7, -5));
  if (id === 0) return json(400, INVALID_ID);
  const exec = () => {
    likeStmt.int(1, user.id).int(2, id).step();
    likeStmt.done();
    const liked = db.changes() === 1;
    commit();
    if (liked) {
      return json(201, `{"liked":true,"already_liked":false,"post_id":${id}}`);
    }
    const exists = existsStmt.int(1, id).step();
    existsStmt.done();
    if (exists) {
      return json(200, `{"liked":true,"already_liked":true,"post_id":${id}}`);
    }
    return json(404, POST_NOT_FOUND);
  };
  return write(exec);
}

function health(): Uint8Array {
  try {
    const s = wBusy ? healthR : healthW;
    s.step();
    s.done();
  } catch (e) {
    return json(
      503,
      JSON.stringify({
        status: "degraded",
        db: "unreachable",
        error: String((e as Error)?.message ?? e),
      }),
    );
  }
  return json(
    200,
    `{"status":"ok","db":"ok","uptime_s":${
      Math.floor(performance.now() / 1000)
    }}`,
  );
}

export function handle(
  method: string,
  path: string,
  auth: string | null,
  body: Uint8Array,
): Resp {
  if (method === "GET") {
    if (path === "/feed") {
      return renderResponse(wBusy ? feedR : feedW, false)!;
    }
    if (path.startsWith("/posts/")) {
      const id = parseId(path.slice(7));
      if (id === 0) return json(400, INVALID_ID);
      return renderResponse(
        (wBusy ? postR : postW).int(1, id),
        true,
      ) ??
        json(404, POST_NOT_FOUND);
    }
    if (path === "/health") return health();
  } else if (method === "POST") {
    if (path === "/posts") return createPost(auth, body);
    if (path.startsWith("/posts/") && path.endsWith("/like")) {
      return likePost(auth, path);
    }
  }
  return json(404, NOT_FOUND);
}

// ---- HTTP/1.1 over raw TCP. Only what the challenge sends: request line, Authorization, Content-Length,
// fixed-length bodies, keep-alive, pipelining. Chunked bodies and Expect are out of scope (SPEC).
const CR = 13, LF = 10, SP = 32, COLON = 58;
const MAX_BUF = 32 * 1024; // headers + a body of at most 16 KB
const AUTHORIZATION = enc.encode("authorization");
const CONTENT_LENGTH = enc.encode("content-length");
const latin1 = new TextDecoder("latin1");
const EMPTY = new Uint8Array(0);

// Case-insensitive header name match; names are ASCII letters and '-', so |0x20 lowercases both.
function isName(
  b: Uint8Array,
  s: number,
  e: number,
  name: Uint8Array,
): boolean {
  if (e - s !== name.length) return false;
  for (let i = 0; i < name.length; i++) {
    if ((b[s + i] | 0x20) !== name[i]) return false;
  }
  return true;
}

let resp: Uint8Array = EMPTY; // response(s) for the current read, set by parse()
// Where parse() puts each response. Default (Deno.listen): appended to resp, one write per read; the poller
// replaces it. A thunk never reaches the default (wBusy stays false there), but running it is the right thing.
let emit = (r: Resp): void => {
  if (typeof r === "function") r = r();
  if (resp.length === 0) resp = r;
  else { // pipelined: one write for the whole batch
    const both = new Uint8Array(resp.length + r.length);
    both.set(resp);
    both.set(r, resp.length);
    resp = both;
  }
};

// Parses one request at b[off..len). Returns bytes consumed, 0 if incomplete, -1 if malformed.
function parse(b: Uint8Array, off: number, len: number): number {
  let i = off;
  while (i < len && b[i] !== SP) i++;
  const mEnd = i++;
  const pStart = i;
  while (i < len && b[i] !== SP && b[i] !== CR) i++;
  const pEnd = i;
  while (i < len && b[i] !== LF) i++;
  if (i >= len) return len - off > 8192 ? -1 : 0;
  i++;
  let auth: string | null = null, bodyLen = 0;
  for (;;) {
    const ls = i;
    while (i < len && b[i] !== LF) i++;
    if (i >= len) return len - off > 16384 ? -1 : 0;
    const le = b[i - 1] === CR ? i - 1 : i;
    i++;
    if (le === ls) break; // blank line: end of headers
    let c = ls;
    while (c < le && b[c] !== COLON) c++;
    let vs = c + 1;
    while (vs < le && b[vs] === SP) vs++;
    if (isName(b, ls, c, AUTHORIZATION)) {
      auth = latin1.decode(b.subarray(vs, le));
    } else if (isName(b, ls, c, CONTENT_LENGTH)) {
      bodyLen = 0;
      for (let k = vs; k < le; k++) {
        const d = b[k] - 48;
        if (d < 0 || d > 9) break;
        bodyLen = bodyLen * 10 + d;
      }
      if (bodyLen > 16384) return -1;
    }
  }
  if (len - i < bodyLen) return 0;
  const m = mEnd - off === 3 && b[off] === 71
    ? "GET"
    : mEnd - off === 4 && b[off] === 80 && b[off + 1] === 79
    ? "POST"
    : "";
  let r: Resp;
  try {
    r = handle(
      m,
      latin1.decode(b.subarray(pStart, pEnd)),
      auth,
      b.subarray(i, i + bodyLen),
    );
  } catch (e) {
    console.error(e);
    r = json(500, '{"error":"internal server error"}');
  }
  emit(r);
  return i + bodyLen - off;
}

async function serveConn(conn: Deno.TcpConn): Promise<void> {
  let buf = new Uint8Array(1024), len = 0;
  try {
    conn.setNoDelay(true);
    for (;;) {
      if (len === buf.length) { // a request that does not fit: grow, up to MAX_BUF
        if (buf.length >= MAX_BUF) return;
        const big = new Uint8Array(buf.length * 4);
        big.set(buf);
        buf = big;
      }
      const n = await conn.read(buf.subarray(len));
      if (n === null) return;
      len += n;
      let off = 0, used: number;
      while (off < len && (used = parse(buf, off, len)) !== 0) {
        if (used < 0) return;
        off += used;
      }
      if (off > 0) {
        buf.copyWithin(0, off, len);
        len -= off;
        if (buf.length > 1024 && len <= 1024) buf = buf.slice(0, 1024); // back to the idle size
      }
      if (resp.length > 0) {
        const out = resp;
        resp = EMPTY;
        for (let w = 0; w < out.length;) {
          w += await conn.write(w === 0 ? out : out.subarray(w));
        }
      }
    }
  } catch {
    // reset by peer, broken pipe: the connection is gone either way
  } finally {
    try {
      conn.close();
    } catch { /* already closed */ }
  }
}

// Linux: the Rust poller (poller.rs, built by build.sh) owns the sockets and the JS thread blocks in px_wait, so
// no request goes through Deno's event loop. Every handler is synchronous, which is what makes that work.
function servePoller(): never {
  const px = Deno.dlopen(new URL("./bin/libpoller.so", import.meta.url), {
    px_start: {
      parameters: ["buffer", "i32", "buffer", "i32", "buffer"],
      result: "i32",
    },
    px_wait: { parameters: ["i32"], result: "i32" },
    px_send: { parameters: ["i32", "buffer", "i32", "i32"], result: "i32" },
    px_crowded: { parameters: [], result: "i32" },
    px_wake_open: { parameters: [], result: "i32" },
    px_gen: { parameters: ["i32"], result: "u32" },
    px_close: { parameters: ["i32"], result: "void" },
  }).symbols;
  const inb = new Uint8Array(1 << 20);
  const idx = new Int32Array(3 * 4096);
  const rc = px.px_start(
    enc.encode(HOST + "\0"),
    PORT,
    inb,
    inb.length,
    new Uint8Array(idx.buffer),
  );
  if (rc !== 0) throw new Error(`px_start ${HOST}:${PORT} failed (${rc})`);
  db.exec("PRAGMA busy_timeout = 0");
  const wake = px.px_wake_open();
  if (wake < 0) throw new Error("px_wake_open: eventfd failed");
  wrStart(db, wake);
  console.log(`listening on ${HOST}:${PORT} (poller)`);
  deferCommit = true;
  // Responses that cannot go out yet, oldest first. A write's (resp set) waits for the COMMIT tagged seq; a
  // write that arrived while db was the commit thread's (exec set, resp null) runs once db is back, then
  // waits for the next COMMIT; and anything for a connection that already has an entry queues behind it, so
  // each connection's responses keep their order. Reads never wait otherwise: rdb shows committed data only.
  // An entry can outlive its connection: poller.rs drops a peer that hangs up and the next accept may reuse
  // its fd. px_gen(fd) changes on every accept, so an entry goes out only to the connection it was for.
  type Held = {
    fd: number;
    gen: number;
    seq: number;
    resp: Uint8Array | null;
    exec: (() => Uint8Array) | null;
    close: boolean; // resp says connection: close (crowded when it was built)
  };
  const held: Held[] = [];
  let first = 0, deepest = 0;
  let seq = 0, done = 0, inFlight = false; // COMMITs handed to the thread / landed / one is in flight
  const heldOn = new Int32Array(65536); // queued entries per fd (poller.rs's MAXFD)
  const heldGen = new Uint32Array(65536); // the connection generation those entries belong to
  // Entries left by a dead connection don't count for the fd's next one (they never send to it either).
  const isHeld = (fd: number) =>
    heldOn[fd] > 0 && heldGen[fd] === px.px_gen(fd);
  const hold = (
    fd: number,
    s: number,
    resp: Uint8Array | null,
    exec: (() => Uint8Array) | null,
  ) => {
    const gen = px.px_gen(fd);
    if (heldGen[fd] !== gen) {
      heldGen[fd] = gen;
      heldOn[fd] = 0;
    }
    held.push({ fd, gen, seq: s, resp, exec, close: crowded });
    heldOn[fd]++;
    if (held.length - first > deepest) deepest = held.length - first;
  };
  const release = () => {
    while (first < held.length) {
      const h = held[first];
      if (h.resp === null || h.seq > done) break;
      first++;
      if (h.gen === heldGen[h.fd]) heldOn[h.fd]--;
      // Reused fd: the old connection is gone, never send. Closed and not reused: px_send returns -1.
      if (px.px_gen(h.fd) === h.gen) {
        px.px_send(h.fd, h.resp, h.resp.length, h.close ? 1 : 0);
      }
    }
    if (first === held.length) held.length = first = 0;
    else if (first > 4096 && 2 * first > held.length) {
      held.splice(0, first);
      first = 0;
    }
  };
  let fd = 0, seenWrites = writes, lastCommits = 0;
  emit = (r) => {
    if (typeof r === "function") hold(fd, 0, null, r); // db busy: runs once it is back
    else if (writes !== seenWrites) hold(fd, seq + 1, r, null); // a write: waits for this batch's COMMIT
    else if (isHeld(fd)) hold(fd, 0, r, null);
    else px.px_send(fd, r, r.length, crowded ? 1 : 0);
    seenWrites = writes;
  };
  for (;;) {
    const n = px.px_wait(1000); // 0 on timeout, or when only the commit thread woke us
    if (n < 0) throw new Error("epoll_wait failed");
    crowded = px.px_crowded() === 1;
    if (inFlight && wrDone() >= seq) { // the COMMIT landed: db is ours again, rdb can see it
      done = seq;
      inFlight = false;
      rStale = true;
      const rc = wrStats().rc;
      if (rc !== 0) { // nothing in it is durable: close those connections rather than answer
        console.error(`COMMIT failed: SQLite error ${rc}`);
        for (let i = first; i < held.length; i++) {
          const h = held[i];
          if (h.seq === done && h.resp !== null) {
            if (px.px_gen(h.fd) === h.gen) px.px_close(h.fd);
            h.resp = EMPTY; // release passes it; px_send finds no connection
          }
        }
      }
      release();
    }
    // A TRUNCATE checkpoint wants the WAL to itself: hold no snapshot on db (nothing uncommitted is on it
    // between batches) and refresh rdb each batch so its reader wait drains. Else rdb sees landed commits
    // at most every 20 ms (each refresh drops its page cache).
    const truncating = ckptWaiting();
    if (truncating) {
      if (!inFlight) {
        run(commitStmt);
        run(beginStmt);
      }
      refreshRead();
    } else if (rStale && performance.now() - lastRefresh >= REFRESH_MS) {
      refreshRead();
    }
    wBusy = inFlight || truncating;
    if (!wBusy) {
      for (let i = first; i < held.length; i++) { // writes that waited for db: run them, into the next COMMIT
        const h = held[i];
        if (h.exec === null) continue;
        try {
          h.resp = h.exec();
        } catch (e) {
          if (busy(e)) { // the checkpointer has the lock or just truncated: next batch
            wBusy = true;
            break;
          }
          console.error(e);
          h.resp = json(500, '{"error":"internal server error"}');
        }
        h.exec = null;
        h.close = crowded;
        h.seq = seq + 1;
      }
      seenWrites = writes;
    }
    for (let e = 0; e < n; e++) {
      fd = idx[3 * e];
      const start = idx[3 * e + 1], end = start + idx[3 * e + 2];
      let off = start, used = 0;
      while (off < end && (used = parse(inb, off, end)) > 0) off += used;
      idx[3 * e + 2] = used < 0 ? -1 : off - start; // bytes consumed; -1 closes it
    }
    if (pending) { // one COMMIT for the batch's writes, on the commit thread; db is its until wrDone moves
      pending = false;
      inFlight = wBusy = true;
      seq++;
      wrCommit();
    }
    if (performance.now() - lastDiag >= 10_000) {
      const w = wrStats(true);
      commits = w.commits - lastCommits;
      lastCommits = w.commits;
      worstCommitMs = w.worstMs;
      diag(` · held max ${deepest}`);
      deepest = 0;
    }
  }
}

if (import.meta.main) {
  if (Deno.build.os === "linux") servePoller();
  // macOS: Deno.listen
  setInterval(() => { // rdb: let a TRUNCATE checkpoint's reader wait drain, see landed commits
    if (ckptWaiting() || rStale) refreshRead();
  }, 100);
  setInterval(diag, 10_000);
  const listener = Deno.listen({
    hostname: HOST,
    port: PORT,
    tcpBacklog: 4096,
  });
  console.log(`listening on ${HOST}:${PORT}`);
  let lastAcceptLog = -Infinity;
  for (;;) {
    let conn: Deno.TcpConn;
    try {
      conn = await listener.accept();
    } catch (e) { // EMFILE and friends: keep the listener, back off briefly, log at most once a second
      if (performance.now() - lastAcceptLog > 1000) {
        lastAcceptLog = performance.now();
        console.error(e);
      }
      await new Promise((r) => setTimeout(r, 10));
      continue;
    }
    serveConn(conn);
  }
}
