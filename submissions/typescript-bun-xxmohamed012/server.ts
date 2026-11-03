// HTTP/1.1 server on Bun.listen and bun:sqlite: one thread, one SQLite connection.
import { Database } from "bun:sqlite";
import { dlopen, FFIType } from "bun:ffi";

const SQLITE_PATH = process.env.SQLITE_PATH;
const JWT_SECRET = process.env.JWT_SECRET;
if (!SQLITE_PATH || !JWT_SECRET) throw new Error("SQLITE_PATH and JWT_SECRET are required");
const HOST = process.env.HOST || "127.0.0.1";
const PORT = Number(process.env.PORT || 3000);

const db = new Database(SQLITE_PATH);
// Single process: take the file lock once instead of per statement.
db.run("PRAGMA locking_mode = EXCLUSIVE");
db.run("PRAGMA journal_mode = WAL");
db.run("PRAGMA synchronous = NORMAL");
db.run("PRAGMA cache_size = -65536"); // 64 MiB
db.run("PRAGMA journal_size_limit = 67108864");

// Rendered by SQLite and read as a BLOB, so the bytes never become a JS string.
const POST_JSON = `json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
  'like_count',(SELECT count(*) FROM likes l WHERE l.post_id = p.id))`;
const feedStmt = db.query<{ b: Uint8Array }, []>(
  `SELECT CAST('{"posts":[' || ifnull(group_concat(j, ','), '') || ']}' AS BLOB) AS b
     FROM (SELECT ${POST_JSON} AS j FROM posts p JOIN users u ON u.id = p.user_id
           ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`,
);
const postStmt = db.query<{ b: Uint8Array }, [number]>(
  `SELECT CAST('{"post":' || ${POST_JSON} || '}' AS BLOB) AS b FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`,
);
const createStmt = db.query<[number, string], [number, string]>(
  "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
);
// 0 changes: already liked, or no such post.
const likeStmt = db.query<never, [number, number]>(
  "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT DO NOTHING",
);
const existsStmt = db.query<{ 1: number }, [number]>("SELECT 1 FROM posts WHERE id = ?");
const healthStmt = db.query("SELECT 1");

const HEAD = {
  200: "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ",
  201: "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: ",
  400: "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: ",
  401: "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ",
  404: "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: ",
  500: "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: ",
  503: "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: ",
};
type Status = keyof typeof HEAD;

const NOT_FOUND = '{"error":"not found"}';
const INVALID_ID = '{"error":"invalid post id"}';
const POST_NOT_FOUND = '{"error":"post not found"}';
const INTERNAL = '{"error":"internal server error"}';
const AUTH_ERRORS = [
  "",
  '{"error":"missing bearer token"}',
  '{"error":"invalid or expired token"}',
  '{"error":"invalid token payload"}',
];

type Conn = { rest: Buffer | null; out: Buffer | null };
type Sock = Bun.Socket<Conn>;

// The body goes at HEAD_ROOM and the headers right before it, so each response is one write().
// socket.write() copies unsent bytes, so OUT can be reused immediately.
const HEAD_ROOM = 128;
const OUT = Buffer.allocUnsafe(1 << 18);

function send(sock: Sock, status: Status, body: string | Uint8Array) {
  let n: number;
  if (typeof body === "string") {
    if (body.length * 3 > OUT.length - HEAD_ROOM) throw new Error("response too large");
    n = OUT.utf8Write(body, HEAD_ROOM);
  } else {
    OUT.set(body, HEAD_ROOM);
    n = body.length;
  }
  const head = HEAD[status] + n + "\r\n\r\n";
  const start = HEAD_ROOM - head.length;
  OUT.latin1Write(head, start);
  const end = HEAD_ROOM + n;
  const conn = sock.data;
  if (conn.out !== null) {
    conn.out = Buffer.concat([conn.out, OUT.subarray(start, end)]); // an earlier response is still queued
    return;
  }
  const written = sock.write(OUT, start, end - start);
  if (written < end - start) conn.out = Buffer.from(OUT.subarray(start + Math.max(written, 0), end));
}

type User = { id: number; username: string };
const MISSING = 1, INVALID = 2, BAD_PAYLOAD = 3;

// Keyed once, cloned per request.
const hmac = new Bun.CryptoHasher("sha256", JWT_SECRET);
// base64url of {"alg":"HS256","typ":"JWT"}; other headers go through headerOk().
const STD_PREFIX = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.";

function b64urlDecode(s: string): string {
  if (s.includes("-")) s = s.replaceAll("-", "+");
  if (s.includes("_")) s = s.replaceAll("_", "/");
  const bin = atob(s); // throws on invalid input
  return /[^\x00-\x7f]/.test(bin) ? Buffer.from(bin, "latin1").toString("utf8") : bin;
}

function headerOk(segment: string): boolean {
  try {
    const h = JSON.parse(b64urlDecode(segment));
    return h !== null && typeof h === "object" && h.alg === "HS256";
  } catch {
    return false;
  }
}

function authenticate(header: string | null): User | number {
  if (header === null || !header.startsWith("Bearer ")) return MISSING;
  const dot2 = header.lastIndexOf(".");
  let dot1: number;
  if (header.startsWith(STD_PREFIX)) dot1 = STD_PREFIX.length - 1;
  else {
    dot1 = header.indexOf(".", 7);
    if (dot1 < 0 || dot1 >= dot2 || !headerOk(header.slice(7, dot1))) return INVALID;
  }
  if (dot2 <= dot1) return INVALID;
  const sig = hmac.copy().update(header.slice(7, dot2)).digest("base64url");
  if (header.length - dot2 - 1 !== sig.length) return INVALID;
  let diff = 0; // constant-time compare
  for (let i = 0; i < sig.length; i++) diff |= sig.charCodeAt(i) ^ header.charCodeAt(dot2 + 1 + i);
  if (diff !== 0) return INVALID;

  let p: any;
  try {
    p = JSON.parse(b64urlDecode(header.slice(dot1 + 1, dot2)));
  } catch {
    return INVALID;
  }
  if (p === null || typeof p !== "object") return BAD_PAYLOAD;
  const { exp, nbf } = p;
  if (exp !== undefined || nbf !== undefined) {
    const now = Date.now() / 1000;
    if (exp !== undefined && (typeof exp !== "number" || exp <= now)) return INVALID;
    if (nbf !== undefined && (typeof nbf !== "number" || nbf > now)) return INVALID;
  }
  const { sub, username } = p;
  if (typeof sub !== "string" || typeof username !== "string") return BAD_PAYLOAD;
  const id = parseId(sub, 0, sub.length);
  return id > 0 && id <= Number.MAX_SAFE_INTEGER ? { id, username } : BAD_PAYLOAD;
}

// Decimal digits only, otherwise 0.
function parseId(s: string, from: number, to: number): number {
  if (from >= to) return 0;
  let n = 0;
  for (let i = from; i < to; i++) {
    const d = s.charCodeAt(i) - 48;
    if (d < 0 || d > 9) return 0;
    n = n * 10 + d;
  }
  return n;
}

// Counts characters like SQLite's length(), not UTF-16 units.
function tooLong(s: string): boolean {
  if (s.length <= 500) return false;
  if (s.length > 1000) return true;
  let chars = s.length;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const d = s.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) { chars--; i++; }
    }
  }
  return chars > 500;
}

function createPost(sock: Sock, auth: string | null, raw: string) {
  const user = authenticate(auth);
  if (typeof user === "number") return send(sock, 401, AUTH_ERRORS[user]);
  let data: any;
  try {
    data = JSON.parse(raw);
  } catch {
    return send(sock, 400, '{"error":"malformed JSON body"}');
  }
  const body = data?.body;
  if (typeof body !== "string") return send(sock, 400, '{"error":"body is required"}');
  const trimmed = body.trim();
  if (trimmed === "") return send(sock, 400, '{"error":"body is required"}');
  if (tooLong(trimmed)) return send(sock, 400, '{"error":"body must be at most 500 characters"}');
  const [id, createdAt] = createStmt.values(user.id, trimmed)[0];
  send(sock, 201, `{"post":{"id":${id},"body":${JSON.stringify(trimmed)},"created_at":"${createdAt}","author":${JSON.stringify(user.username)},"like_count":0}}`);
}

function likePost(sock: Sock, auth: string | null, target: string) {
  const user = authenticate(auth); // auth before id (SPEC.md)
  if (typeof user === "number") return send(sock, 401, AUTH_ERRORS[user]);
  const id = parseId(target, 7, target.length - 5);
  if (id === 0) return send(sock, 400, INVALID_ID);
  if (likeStmt.run(user.id, id).changes === 1) {
    return send(sock, 201, `{"liked":true,"already_liked":false,"post_id":${id}}`);
  }
  if (existsStmt.get(id) !== null) return send(sock, 200, `{"liked":true,"already_liked":true,"post_id":${id}}`);
  send(sock, 404, POST_NOT_FOUND);
}

function health(sock: Sock) {
  try {
    healthStmt.get();
  } catch (e) {
    return send(sock, 503, JSON.stringify({ status: "degraded", db: "unreachable", error: String((e as Error)?.message ?? e) }));
  }
  send(sock, 200, `{"status":"ok","db":"ok","uptime_s":${Math.floor(performance.now() / 1000)}}`);
}

// method: first byte of the request line, 'G' (71) or 'P' (80).
function route(sock: Sock, method: number, target: string, auth: string | null, body: string) {
  if (method === 71) {
    if (target === "/feed") return send(sock, 200, feedStmt.get()!.b);
    if (target.startsWith("/posts/")) {
      const id = parseId(target, 7, target.length);
      if (id === 0) return send(sock, 400, INVALID_ID);
      const row = postStmt.get(id);
      return row === null ? send(sock, 404, POST_NOT_FOUND) : send(sock, 200, row.b);
    }
    if (target === "/health") return health(sock);
  } else if (method === 80) {
    if (target === "/posts") return createPost(sock, auth, body);
    if (target.startsWith("/posts/") && target.endsWith("/like")) return likePost(sock, auth, target);
  }
  send(sock, 404, NOT_FOUND);
}

const CRLF2 = Buffer.from("\r\n\r\n");
const MAX_HEAD = 16384;
const MAX_BODY = 65536;

// Case-insensitive prefix match against a lowercase ASCII header name.
function startsWithCI(buf: Buffer, at: number, name: string): boolean {
  for (let i = 0; i < name.length; i++) if ((buf[at + i] | 32) !== name.charCodeAt(i)) return false;
  return true;
}

// Past HIGH_WATER open connections (LimitNOFILE is 65535), close the least recently used ones; clients
// reconnect on their next request. RST instead of FIN: no TIME_WAIT, and conntrack entries expire in 10 s.
const HIGH_WATER = 64_000;
const SHED_TO = 63_000;
const conns = new Set<Sock>(); // least recently used first

function shed() {
  for (const sock of conns) {
    if (conns.size <= SHED_TO) return;
    const conn = sock.data;
    if (conn.rest !== null || conn.out !== null) continue; // request or response in progress
    conns.delete(sock);
    sock.terminate();
  }
}

function onData(sock: Sock, chunk: Buffer) {
  const conn = sock.data;
  conns.delete(sock); // move to the most recently active end
  conns.add(sock);
  let buf = chunk;
  if (conn.rest !== null) {
    buf = Buffer.concat([conn.rest, chunk]);
    conn.rest = null;
  }
  const n = buf.length;
  let pos = 0;
  while (pos < n) { // a chunk can hold several pipelined requests
    const headEnd = buf.indexOf(CRLF2, pos);
    if (headEnd < 0) {
      if (n - pos > MAX_HEAD) return void sock.end();
      break;
    }
    const sp1 = buf.indexOf(32, pos);
    const sp2 = sp1 < 0 ? -1 : buf.indexOf(32, sp1 + 1);
    if (sp2 < 0 || sp2 > headEnd) return void sock.end();
    const method = buf[pos];
    const target = buf.latin1Slice(sp1 + 1, sp2);

    let contentLength = 0;
    let auth: string | null = null;
    let close = false;
    let line = buf.indexOf(10, sp2) + 1;
    while (line < headEnd + 2) {
      const eol = buf.indexOf(10, line);
      const c = buf[line] | 32;
      if (c === 99 /* c */) {
        if (startsWithCI(buf, line, "content-length:")) {
          let i = line + 15;
          while (buf[i] === 32 || buf[i] === 9) i++;
          for (let d = buf[i] - 48; d >= 0 && d <= 9; d = buf[++i] - 48) contentLength = contentLength * 10 + d;
        } else if (startsWithCI(buf, line, "connection:")) {
          let i = line + 11;
          while (buf[i] === 32 || buf[i] === 9) i++;
          close = startsWithCI(buf, i, "close");
        }
      } else if (c === 97 /* a */ && startsWithCI(buf, line, "authorization:")) {
        let i = line + 14;
        while (buf[i] === 32 || buf[i] === 9) i++;
        auth = buf.latin1Slice(i, eol - 1);
      }
      line = eol + 1;
    }
    if (contentLength > MAX_BODY) return void sock.end();
    const bodyStart = headEnd + 4;
    if (n < bodyStart + contentLength) break; // wait for the rest of the body

    try {
      route(sock, method, target, auth, contentLength > 0 ? buf.utf8Slice(bodyStart, bodyStart + contentLength) : "");
    } catch (e) {
      console.error(e);
      send(sock, 500, INTERNAL);
    }
    pos = bodyStart + contentLength;
    if (close) return void sock.end();
  }
  if (pos < n) conn.rest = Buffer.from(buf.subarray(pos));
}

const listener = Bun.listen<Conn>({
  hostname: HOST,
  port: PORT,
  socket: {
    open(sock) {
      // No idle timeout: socket.timeout() isn't reset by activity. Bun already sets TCP_NODELAY.
      sock.data = { rest: null, out: null };
      conns.add(sock);
      if (conns.size > HIGH_WATER) shed();
    },
    data: onData,
    close(sock) {
      conns.delete(sock);
    },
    drain(sock) {
      const conn = sock.data;
      if (conn.out === null) return;
      const written = sock.write(conn.out);
      conn.out = written < conn.out.length ? conn.out.subarray(Math.max(written, 0)) : null;
    },
    error(_sock, err) {
      console.error(err);
    },
  },
});

// Bun's backlog is fixed at 512, about 20 ms of reconnects under shedding. A second listen() resizes it
// (capped by somaxconn).
const libc = dlopen("libc.so.6", { listen: { args: [FFIType.i32, FFIType.i32], returns: FFIType.i32 } });
if (libc.symbols.listen(listener.fd, 4096) !== 0) console.error("could not raise the listen backlog; keeping 512");
console.log(`listening on ${HOST}:${PORT}`);
