'use strict';
// The $12 server challenge in plain Node: node:net + node:sqlite + node:crypto, no dependencies.
// HTTP/1.1 is parsed by hand on raw TCP sockets: node:http builds request/response objects, streams
// and events for every request, and that was most of our CPU once SQLite was fast.
const net = require('node:net');
const { createHmac, timingSafeEqual } = require('node:crypto');
const { DatabaseSync } = require('node:sqlite');

const { SQLITE_PATH, JWT_SECRET, HOST = '127.0.0.1', PORT = '3000' } = process.env;
if (!SQLITE_PATH || !JWT_SECRET) {
  console.error('SQLITE_PATH and JWT_SECRET are required');
  process.exit(1);
}

// ---------- database ----------
const db = new DatabaseSync(SQLITE_PATH);
db.exec(`
  PRAGMA journal_mode = WAL;
  PRAGMA synchronous = NORMAL;
  PRAGMA busy_timeout = 5000;
  PRAGMA cache_size = -65536;
  PRAGMA mmap_size = 268435456;
  PRAGMA temp_store = MEMORY;
`);

// SQLite builds the whole response body for reads: one string crosses into JS instead of row
// objects, which saves allocations (and GC). json_object keeps keys in the order given.
const POST_ROWS = `
  SELECT p.id, p.body, p.created_at, u.username,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id`;
const POST_JSON = `json_object('id', id, 'body', body, 'created_at', created_at, 'author', username, 'like_count', like_count)`;
// Returns each row as an array, so a single-column row is just [value].
const prepare = (sql) => { const s = db.prepare(sql); s.setReturnArrays(true); return s; };
const q = {
  ping: db.prepare('SELECT 1'),
  feed: prepare(`
    SELECT '{"posts":' || json_group_array(${POST_JSON} ORDER BY created_at DESC, id DESC) || '}'
      FROM (${POST_ROWS} ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`),
  post: prepare(`SELECT '{"post":' || ${POST_JSON} || '}' FROM (${POST_ROWS} WHERE p.id = ?)`),
  createPost: prepare('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at'),
  // Inserts only if the post exists; 0 changes means "already liked" or "no such post".
  like: db.prepare(`INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ?
                    ON CONFLICT (user_id, post_id) DO NOTHING`),
  postExists: prepare('SELECT 1 FROM posts WHERE id = ?'),
};

// Handlers return [status, body].
const error = (status, message) => [status, `{"error":"${message}"}`];

// ---------- auth (HS256 JWT) ----------
const SECRET = Buffer.from(JWT_SECRET);
const POSITIVE_INT = /^[1-9][0-9]*$/;
const parseId = (s) => (POSITIVE_INT.test(s) ? Number(s) : 0);

// Returns { userId, username }, or the 401 message when the token is no good.
function authenticate(header) {
  if (!header || !header.startsWith('Bearer ')) return 'missing bearer token';
  let payload;
  try {
    const token = header.slice(7);
    const dot1 = token.indexOf('.');
    const dot2 = token.indexOf('.', dot1 + 1);
    if (dot1 < 0 || dot2 < 0 || token.indexOf('.', dot2 + 1) >= 0) throw 0;
    const head = JSON.parse(Buffer.from(token.slice(0, dot1), 'base64url'));
    if (head.alg !== 'HS256') throw 0;
    const sig = Buffer.from(token.slice(dot2 + 1), 'base64url');
    const expected = createHmac('sha256', SECRET).update(token.slice(0, dot2)).digest();
    if (sig.length !== expected.length || !timingSafeEqual(sig, expected)) throw 0;
    payload = JSON.parse(Buffer.from(token.slice(dot1 + 1, dot2), 'base64url'));
    if (payload === null || typeof payload !== 'object') throw 0;
    if (payload.exp !== undefined && !(typeof payload.exp === 'number' && payload.exp > Date.now() / 1000)) throw 0;
  } catch {
    return 'invalid or expired token';
  }
  const { sub, username } = payload;
  const userId = typeof sub === 'string' ? parseId(sub) : 0;
  if (!userId || !Number.isSafeInteger(userId) || typeof username !== 'string') return 'invalid token payload';
  return { userId, username };
}

// ---------- handlers ----------
const startedAt = Date.now();

function health() {
  try {
    q.ping.get();
  } catch (err) {
    return [503, `{"status":"degraded","db":"unreachable","error":${JSON.stringify(String(err.message))}}`];
  }
  return [200, `{"status":"ok","db":"ok","uptime_s":${Math.floor((Date.now() - startedAt) / 1000)}}`];
}

function feed() {
  return [200, q.feed.get()[0]];
}

function getPost(rawId) {
  const id = parseId(rawId);
  if (!id) return error(400, 'invalid post id');
  const row = q.post.get(id);
  if (!row) return error(404, 'post not found');
  return [200, row[0]];
}

function createPost(user, raw) {
  let data;
  try {
    data = JSON.parse(raw);
  } catch {
    return error(400, 'malformed JSON body');
  }
  const body = data !== null && typeof data === 'object' && typeof data.body === 'string' ? data.body.trim() : '';
  if (!body) return error(400, 'body is required');
  // "characters" = code points (what SQLite's length() counts), not UTF-16 units.
  if (body.length > 500 && [...body].length > 500) return error(400, 'body must be at most 500 characters');
  const [id, createdAt] = q.createPost.get(user.userId, body);
  // Key order is fixed by the spec, so build the JSON by hand.
  return [201,
    `{"post":{"id":${id},"body":${JSON.stringify(body)},"created_at":"${createdAt}",` +
    `"author":${JSON.stringify(user.username)},"like_count":0}}`];
}

function likePost(user, rawId) {
  const id = parseId(rawId);
  if (!id) return error(400, 'invalid post id');
  if (q.like.run(user.userId, id).changes === 1) {
    return [201, `{"liked":true,"already_liked":false,"post_id":${id}}`];
  }
  if (!q.postExists.get(id)) return error(404, 'post not found');
  return [200, `{"liked":true,"already_liked":true,"post_id":${id}}`];
}

// ---------- router ----------
function route(method, target, auth, body) {
  const qs = target.indexOf('?');
  const path = qs < 0 ? target : target.slice(0, qs);

  if (method === 'GET') {
    if (path === '/feed') return feed();
    if (path === '/health') return health();
    if (path.startsWith('/posts/')) {
      const id = path.slice(7);
      if (id && !id.includes('/')) return getPost(id);
    }
  } else if (method === 'POST') {
    if (path === '/posts') {
      const user = authenticate(auth);
      return typeof user === 'string' ? error(401, user) : createPost(user, body);
    }
    if (path.startsWith('/posts/') && path.endsWith('/like')) {
      const id = path.slice(7, -5);
      if (id && !id.includes('/')) {
        const user = authenticate(auth);
        return typeof user === 'string' ? error(401, user) : likePost(user, id);
      }
    }
  }
  return error(404, 'not found');
}

// ---------- HTTP/1.1 ----------
const IDLE_MS = 70_000; // drop a connection after 70 s of silence: longer than any k6 think time
const MAX_HEAD = 8 * 1024;
const MAX_BODY = 64 * 1024;
const REASON = {
  200: 'OK', 201: 'Created', 400: 'Bad Request', 401: 'Unauthorized', 404: 'Not Found',
  411: 'Length Required', 413: 'Content Too Large', 431: 'Request Header Fields Too Large',
  500: 'Internal Server Error', 503: 'Service Unavailable',
};

// HTTP requires a Date header. Formatting it once a second is the clock, not a response cache.
let date = new Date().toUTCString();
setInterval(() => { date = new Date().toUTCString(); }, 1000).unref();

function response([status, body], close) {
  return `HTTP/1.1 ${status} ${REASON[status]}\r\nContent-Type: application/json\r\n` +
    `Content-Length: ${Buffer.byteLength(body)}\r\nDate: ${date}\r\n` +
    `${close ? 'Connection: close\r\n' : ''}\r\n${body}`;
}

function handle(req, body) {
  let res;
  try {
    res = route(req.method, req.target, req.auth, body);
  } catch (err) {
    console.error(err);
    res = error(500, 'internal server error');
  }
  return response(res, req.close);
}

// Parses the request line and the headers we use. Returns null if the request is malformed.
function parseHead(head) {
  const lineEnd = head.indexOf('\r\n') < 0 ? head.length : head.indexOf('\r\n');
  const sp1 = head.indexOf(' ');
  const sp2 = head.indexOf(' ', sp1 + 1);
  if (sp1 <= 0 || sp2 < 0 || sp2 + 1 >= lineEnd) return null;
  const version = head.slice(sp2 + 1, lineEnd);
  if (version !== 'HTTP/1.1' && version !== 'HTTP/1.0') return null;
  const req = {
    method: head.slice(0, sp1), target: head.slice(sp1 + 1, sp2), auth: undefined,
    length: -1, chunked: false, expect: false, close: version === 'HTTP/1.0', // 1.0 closes unless asked not to
  };
  for (let at = lineEnd; at < head.length;) {
    const start = at + 2;
    let end = head.indexOf('\r\n', start);
    if (end < 0) end = head.length;
    const colon = head.indexOf(':', start);
    if (colon <= start || colon > end) return null;
    const value = head.slice(colon + 1, end).trim();
    switch (head.slice(start, colon).toLowerCase()) {
      case 'authorization': req.auth ??= value; break;
      case 'content-length': {
        if (!/^[0-9]{1,9}$/.test(value)) return null;
        const n = Number(value);
        if (req.length >= 0 && req.length !== n) return null;
        req.length = n;
        break;
      }
      case 'transfer-encoding': req.chunked = true; break;
      case 'connection': {
        const v = value.toLowerCase();
        if (v.includes('close')) req.close = true;
        else if (v.includes('keep-alive')) req.close = false;
        break;
      }
      case 'expect': req.expect = value.toLowerCase() === '100-continue'; break;
    }
    at = end;
  }
  if (req.length < 0) req.length = 0;
  return req;
}

function onConnection(socket) {
  let buf = null;       // bytes received but not yet handled
  let closing = false;  // we've sent Connection: close
  let continued = false; // sent "100 Continue" for the request we're waiting on
  socket.setNoDelay(true);
  socket.setTimeout(IDLE_MS, () => socket.destroy());
  socket.on('error', () => socket.destroy());
  socket.on('data', function onData(chunk) {
    if (closing) return;
    buf = buf === null ? chunk : Buffer.concat([buf, chunk]);
    // Handle every complete request in the buffer (pipelining), then send all responses in one write.
    // Handlers are synchronous, so responses come out in request order.
    let out = '';
    let pos = 0;
    while (pos < buf.length) {
      const headEnd = buf.indexOf('\r\n\r\n', pos, 'latin1');
      if (headEnd < 0) {
        if (buf.length - pos > MAX_HEAD) { out += response(error(431, 'request headers too large'), true); closing = true; }
        break;
      }
      const req = parseHead(buf.toString('latin1', pos, headEnd));
      if (req === null) { out += response(error(400, 'bad request'), true); closing = true; break; }
      // Bodies always come with Content-Length in practice; chunked uploads aren't supported.
      if (req.chunked) { out += response(error(411, 'content-length required'), true); closing = true; break; }
      if (req.length > MAX_BODY) { out += response(error(413, 'body too large'), true); closing = true; break; }
      const bodyStart = headEnd + 4;
      if (buf.length < bodyStart + req.length) {
        if (req.expect && !continued) { out += 'HTTP/1.1 100 Continue\r\n\r\n'; continued = true; }
        break;
      }
      continued = false;
      out += handle(req, req.length ? buf.toString('utf8', bodyStart, bodyStart + req.length) : '');
      pos = bodyStart + req.length;
      if (req.close) { closing = true; break; }
    }
    buf = pos < buf.length ? buf.subarray(pos) : null;
    if (out) {
      if (closing) socket.end(out);
      // A client that sends faster than it reads waits for its responses to drain.
      else if (!socket.write(out)) { socket.pause(); socket.once('drain', () => socket.resume()); }
    }
  });
}

const server = net.createServer(onConnection);
server.listen({ port: Number(PORT), host: HOST, backlog: 4096 }, () => console.log(`listening on ${HOST}:${PORT}`));

const shutdown = () => { server.close(); db.close(); process.exit(0); };
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
