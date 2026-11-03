'use strict';
// The $12 server challenge on Node.js: uWebSockets.js for HTTP, better-sqlite3 for SQLite, one thread.
//
// - Reads run synchronously inside the uWS handler. SQLite builds the whole JSON body, so a single
//   string crosses into JS per request and no row objects are created.
// - Writes are group-committed: they are queued and executed at the end of the current event-loop
//   turn inside one transaction. Every write response is sent only after COMMIT returns. Reads never
//   run while that transaction is open, so they never see uncommitted rows.
const path = require('node:path');
const fs = require('node:fs');
const { createHmac, createSecretKey, timingSafeEqual } = require('node:crypto');
const uWS = require('./bin/uws.node');
const Database = require('better-sqlite3');

const { SQLITE_PATH, JWT_SECRET, HOST = '127.0.0.1', PORT = '3000' } = process.env;
if (!SQLITE_PATH || !JWT_SECRET) {
  console.error('SQLITE_PATH and JWT_SECRET are required');
  process.exit(1);
}

// ---------------------------------------------------------------- database
const db = new Database(SQLITE_PATH, { nativeBinding: path.join(__dirname, 'bin', 'better_sqlite3.node') });
db.pragma('journal_mode = WAL');
db.pragma('synchronous = NORMAL'); // rule 6: WAL + NORMAL, a COMMIT is durable once it returns
db.pragma('busy_timeout = 5000');
db.pragma('mmap_size = 1073741824'); // read the 240 MB file through the OS page cache, no copies
db.pragma('cache_size = -32768'); // 32 MiB for pages that don't come from mmap (WAL frames)
db.pragma('temp_store = MEMORY');
db.pragma('foreign_keys = OFF'); // SQLite's default; better-sqlite3 turns it on. The schema has no ON DELETE actions
// This process is the only user of the database, so take the lock once and keep it: the WAL index lives
// in heap memory instead of a shared-memory file, and no read or write transaction pays for file locks.
db.pragma('locking_mode = EXCLUSIVE');
// Checkpoint every 4000 pages (16 MB of WAL) instead of 1000: each checkpoint costs two fsyncs that block
// the event loop, and pages written again in the meantime are copied to the database file only once.
db.pragma('wal_autocheckpoint = 4000');

const POST_JSON = `json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
  'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))`;
const prepare = (sql) => db.prepare(sql).pluck(); // every query returns one column: the value itself

const q = {
  ping: prepare('SELECT 1'),
  // The subquery hands rows to the aggregate in its ORDER BY order (a co-routine scanning the
  // (created_at DESC, id DESC) index), so group_concat needs no ORDER BY of its own.
  feed: prepare(`
    SELECT '{"posts":[' || group_concat(j, ',') || ']}' FROM (
      SELECT ${POST_JSON} AS j
        FROM posts p JOIN users u ON u.id = p.user_id
       ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`),
  post: prepare(`SELECT '{"post":' || ${POST_JSON} || '}' FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`),
  createPost: prepare(`
    INSERT INTO posts (user_id, body) VALUES (?, ?)
    RETURNING '{"post":' || json_object('id', id, 'body', body, 'created_at', created_at, 'author', ?, 'like_count', 0) || '}'`),
  // Inserts only if the post exists: 0 changes means "already liked" or "no such post".
  like: db.prepare('INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT DO NOTHING'),
  postExists: prepare('SELECT 1 FROM posts WHERE id = ?'),
  begin: db.prepare('BEGIN'),
  commit: db.prepare('COMMIT'),
  rollback: db.prepare('ROLLBACK'),
};

// ---------------------------------------------------------------- responses
const S200 = '200 OK', S201 = '201 Created', S400 = '400 Bad Request', S401 = '401 Unauthorized';
const S404 = '404 Not Found', S500 = '500 Internal Server Error', S503 = '503 Service Unavailable';
const err = (message) => `{"error":"${message}"}`;
const E_NOT_FOUND = err('not found'), E_BAD_ID = err('invalid post id'), E_NO_POST = err('post not found');
const E_NO_TOKEN = err('missing bearer token'), E_BAD_TOKEN = err('invalid or expired token');
const E_BAD_PAYLOAD = err('invalid token payload'), E_BAD_JSON = err('malformed JSON body');
const E_NO_BODY = err('body is required'), E_LONG_BODY = err('body must be at most 500 characters');
const E_INTERNAL = err('internal server error');
// Header names/values as bytes: uWS takes them without a string conversion.
const CONTENT_TYPE = Buffer.from('Content-Type'), APPLICATION_JSON = Buffer.from('application/json');

// Connections above this count get "Connection: close" after their response, so the process never
// reaches its descriptor limit (uSockets would spin on EMFILE). Node raises the soft limit to the hard
// one at startup; read what we got and keep headroom for SQLite and the event loop.
const KEEP_ALIVE_BUDGET = (() => {
  try {
    const m = /^Max open files\s+(\d+)/m.exec(fs.readFileSync('/proc/self/limits', 'utf8'));
    if (m) return Math.max(64, Number(m[1]) - 1024);
  } catch {}
  return 60000;
})();
let connections = 0;

function send(res, status, body) {
  if (status !== S200) res.writeStatus(status); // uWS writes "200 OK" itself when no status was set
  res.writeHeader(CONTENT_TYPE, APPLICATION_JSON).end(body, connections > KEEP_ALIVE_BUDGET);
}

// A positive integer in plain decimal digits, else 0. Values past 2^53 come back inexact, and the
// callers treat anything above Number.MAX_SAFE_INTEGER as a post that can't exist.
function parseId(s) {
  const n = s.length;
  let v = 0;
  for (let i = 0; i < n; i++) {
    const c = s.charCodeAt(i) - 48;
    if (c < 0 || c > 9) return 0;
    v = v * 10 + c;
  }
  return v;
}

// ---------------------------------------------------------------- auth (HS256 JWT, verified every time)
const SECRET = createSecretKey(Buffer.from(JWT_SECRET)); // a KeyObject: createHmac won't re-import a raw key per call
const HS256_HEADER = Buffer.from('{"alg":"HS256","typ":"JWT"}').toString('base64url');

// Returns { id, name } or one of the 401 bodies.
function authenticate(header) {
  if (header.length < 8 || !header.startsWith('Bearer ')) return E_NO_TOKEN;
  const dot1 = header.indexOf('.', 7);
  const dot2 = dot1 < 0 ? -1 : header.indexOf('.', dot1 + 1);
  if (dot2 < 0 || header.indexOf('.', dot2 + 1) >= 0) return E_BAD_TOKEN;
  let payload;
  try {
    const head = header.slice(7, dot1);
    if (head !== HS256_HEADER) { // the common header verbatim, else decode it
      const h = JSON.parse(Buffer.from(head, 'base64url').toString());
      if (h === null || typeof h !== 'object' || h.alg !== 'HS256') return E_BAD_TOKEN;
    }
    const sig = Buffer.from(header.slice(dot2 + 1), 'base64url');
    const mac = createHmac('sha256', SECRET).update(header.slice(7, dot2)).digest();
    if (sig.length !== 32 || !timingSafeEqual(sig, mac)) return E_BAD_TOKEN;
    payload = JSON.parse(Buffer.from(header.slice(dot1 + 1, dot2), 'base64url').toString());
  } catch {
    return E_BAD_TOKEN;
  }
  if (payload === null || typeof payload !== 'object') return E_BAD_TOKEN;
  const now = Date.now() / 1000;
  const { exp, nbf, sub, username } = payload;
  if (exp !== undefined && !(typeof exp === 'number' && now < exp)) return E_BAD_TOKEN;
  if (nbf !== undefined && !(typeof nbf === 'number' && now >= nbf)) return E_BAD_TOKEN;
  const id = typeof sub === 'string' ? parseId(sub) : 0;
  if (!(id > 0 && id <= Number.MAX_SAFE_INTEGER) || typeof username !== 'string') return E_BAD_PAYLOAD;
  return { id, name: username };
}

// ---------------------------------------------------------------- group commit
// A queued write: run() executes its statement inside the batch transaction and sets status/body.
class Write {
  constructor(res, kind, userId, arg, name) {
    this.res = res; this.kind = kind; this.userId = userId; this.arg = arg; this.name = name;
    this.status = S500; this.body = E_INTERNAL; this.aborted = false;
  }
  run() {
    if (this.kind === 0) {
      this.body = q.createPost.get(this.userId, this.arg, this.name);
      this.status = S201;
    } else if (q.like.run(this.userId, this.arg).changes === 1) {
      this.status = S201; this.body = `{"liked":true,"already_liked":false,"post_id":${this.arg}}`;
    } else if (q.postExists.get(this.arg) !== undefined) {
      this.status = S200; this.body = `{"liked":true,"already_liked":true,"post_id":${this.arg}}`;
    } else {
      this.status = S404; this.body = E_NO_POST;
    }
  }
}

let queue = [];
let flushScheduled = false;

// setImmediate runs right after the poll phase, so a batch is every write that arrived in one event-loop
// turn. (A timer window that waits for more writes measured no CPU win at k6 rates, only latency.)
function schedule(w) {
  queue.push(w);
  if (!flushScheduled) { flushScheduled = true; setImmediate(flush); }
}

function fail(w) { w.status = S500; w.body = E_INTERNAL; }

// Logs at most one error per second, so a failing disk can't flood the log.
let lastLog = 0;
function logError(e) {
  const now = Date.now();
  if (now - lastLog >= 1000) { lastLog = now; console.error(new Date(now).toISOString(), e); }
}

function flush() {
  flushScheduled = false;
  const batch = queue;
  queue = [];
  let committed = 0; // batch[0..committed) are settled (committed or failed on their own)
  try {
    q.begin.run();
    for (let i = 0; i < batch.length; i++) {
      try {
        batch[i].run();
      } catch (e) {
        logError(e);
        fail(batch[i]);
        if (!db.inTransaction) { // an I/O-class error rolled the whole transaction back
          for (let j = committed; j < i; j++) fail(batch[j]);
          committed = i + 1;
          q.begin.run();
        }
      }
    }
    q.commit.run();
  } catch (e) {
    logError(e);
    if (db.inTransaction) try { q.rollback.run(); } catch {}
    for (let j = committed; j < batch.length; j++) fail(batch[j]);
  }
  for (let i = 0; i < batch.length; i++) {
    const w = batch[i];
    if (!w.aborted) try { w.res.cork(() => send(w.res, w.status, w.body)); } catch (e) { logError(e); }
  }
}

// ---------------------------------------------------------------- handlers
const startedAt = Date.now();

function health(res) {
  try {
    q.ping.get();
  } catch (e) {
    return send(res, S503, `{"status":"degraded","db":"unreachable","error":${JSON.stringify(String(e && e.message))}}`);
  }
  send(res, S200, `{"status":"ok","db":"ok","uptime_s":${Math.floor((Date.now() - startedAt) / 1000)}}`);
}

function feed(res) {
  send(res, S200, q.feed.get() ?? '{"posts":[]}'); // group_concat over no rows is NULL
}

function getPost(res, req) {
  const id = parseId(req.getParameter(0));
  if (id === 0) return send(res, S400, E_BAD_ID);
  const json = id <= Number.MAX_SAFE_INTEGER ? q.post.get(id) : undefined;
  if (json === undefined) return send(res, S404, E_NO_POST);
  send(res, S200, json);
}

function likePost(res, req) {
  const user = authenticate(req.getHeader('authorization'));
  if (typeof user === 'string') return send(res, S401, user);
  const id = parseId(req.getParameter(0));
  if (id === 0) return send(res, S400, E_BAD_ID);
  if (id > Number.MAX_SAFE_INTEGER) return send(res, S404, E_NO_POST);
  const w = new Write(res, 1, user.id, id, null);
  res.onAborted(() => { w.aborted = true; });
  schedule(w);
}

const MAX_BODY = 64 * 1024;

function createPost(res, req) {
  const user = authenticate(req.getHeader('authorization'));
  if (typeof user === 'string') return send(res, S401, user);
  let chunks = null, size = 0, aborted = false, write = null;
  res.onAborted(() => { aborted = true; if (write !== null) write.aborted = true; });
  res.onData((ab, isLast) => {
    if (aborted) return;
    try {
      if (size + ab.byteLength > MAX_BODY) {
        aborted = true;
        return res.cork(() => res.writeStatus('413 Payload Too Large').writeHeader(CONTENT_TYPE, APPLICATION_JSON).end(err('body too large'), true));
      }
      if (!isLast || chunks !== null) {
        (chunks ??= []).push(Buffer.from(ab.slice(0))); // ab is only valid during this callback
        size += ab.byteLength;
        if (!isLast) return;
      }
      const raw = chunks === null ? Buffer.from(ab).toString() : Buffer.concat(chunks, size).toString();
      let data;
      try {
        data = JSON.parse(raw);
      } catch {
        return res.cork(() => send(res, S400, E_BAD_JSON));
      }
      const body = data !== null && typeof data === 'object' && typeof data.body === 'string' ? data.body.trim() : '';
      if (body === '') return res.cork(() => send(res, S400, E_NO_BODY));
      // "characters" are code points (what SQLite's length() counts), not UTF-16 units
      if (body.length > 500 && countCodePoints(body) > 500) return res.cork(() => send(res, S400, E_LONG_BODY));
      write = new Write(res, 0, user.id, body, user.name);
      schedule(write);
    } catch (e) {
      logError(e);
      aborted = true;
      try { res.cork(() => send(res, S500, E_INTERNAL)); } catch {}
    }
  });
}

function countCodePoints(s) {
  let n = 0;
  for (let i = 0; i < s.length; i++, n++) {
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
      const d = s.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) i++;
    }
  }
  return n;
}

// Any exception escaping a handler becomes a 500 rather than killing the process.
const guard = (fn) => (res, req) => {
  try {
    fn(res, req);
  } catch (e) {
    logError(e);
    try { res.cork(() => send(res, S500, E_INTERNAL)); } catch {}
  }
};

// ---------------------------------------------------------------- server
const app = uWS.App()
  .get('/feed', guard(feed))
  .get('/posts/:id', guard(getPost))
  .post('/posts/:id/like', guard(likePost))
  .post('/posts', guard(createPost))
  .get('/health', guard(health))
  .any('/*', (res) => send(res, S404, E_NOT_FOUND))
  .filter((res, count) => { connections += count; });

// Warm the page cache and the statements before taking traffic.
q.feed.get(); q.post.get(1);

// LIBUS_LISTEN_EXCLUSIVE_PORT (1): no SO_REUSEPORT, so a second instance fails instead of sharing the port.
app.listen(HOST, Number(PORT), 1, (token) => {
  if (!token) {
    console.error(`failed to listen on ${HOST}:${PORT}`);
    process.exit(1);
  }
  console.log(`listening on ${HOST}:${PORT} (keep-alive budget ${KEEP_ALIVE_BUDGET} connections)`);
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    if (queue.length) flush();
    try { db.close(); } catch {}
    process.exit(0);
  });
}
