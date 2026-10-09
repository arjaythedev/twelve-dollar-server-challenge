import { Database } from "bun:sqlite";
import { timingSafeEqual } from "node:crypto";
import { connectionPressure } from "./pressure";

const { SQLITE_PATH, JWT_SECRET, HOST = "127.0.0.1", PORT = "3000" } = process.env;
if (!SQLITE_PATH || !JWT_SECRET) throw new Error("SQLITE_PATH and JWT_SECRET are required");
const started = performance.now();
const db = new Database(SQLITE_PATH, { strict: true });
db.exec(`PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL;
  PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;
  PRAGMA cache_size=-65536; PRAGMA mmap_size=268435456;`);

// Every request reads fresh SQLite rows, in the required JSON key order.
const postSQL = `SELECT p.id,p.body,p.created_at,u.username AS author,
  (SELECT count(*) FROM likes WHERE post_id=p.id) AS like_count
  FROM posts p JOIN users u ON u.id=p.user_id`;
const feed = db.query(`${postSQL} ORDER BY p.created_at DESC,p.id DESC LIMIT 20`);
const post = db.query(`${postSQL} WHERE p.id=?`);
const create = db.query<{ id: number; created_at: string }, [number, string]>(
  "INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at",
);
const like = db.query(`INSERT INTO likes(user_id,post_id)
  SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2)
  ON CONFLICT(user_id,post_id) DO NOTHING`);
const exists = db.query("SELECT 1 FROM posts WHERE id=?");
const health = db.query("SELECT 1");

const json = (body: unknown, status = 200) => Response.json(body, {
  status, headers: connectionPressure() ? { Connection: "close" } : undefined,
});
const error = (status: number, message: string) => json({ error: message }, status);
const positiveID = (s: string) => /^\d+$/.test(s) && Number.isSafeInteger(+s) && +s > 0 ? +s : 0;
const hmac = new Bun.CryptoHasher("sha256", JWT_SECRET);
const utf8 = new TextDecoder("utf-8", { fatal: true });
const decode = (s: string) => JSON.parse(utf8.decode(Buffer.from(s, "base64url")));

function authenticate(req: Request): { id: number; username: string } | Response {
  const auth = req.headers.get("authorization");
  if (!auth?.startsWith("Bearer ")) return error(401, "missing bearer token");
  let payload;
  try {
    const parts = /^([\w-]+)\.([\w-]+)\.([\w-]+)$/.exec(auth.slice(7));
    if (!parts || parts.slice(1).some(s => s.length % 4 === 1)) throw 0;
    const [, header, claims, signature] = parts;
    if (decode(header)?.alg !== "HS256") throw 0;
    const expected = hmac.copy().update(`${header}.${claims}`).digest();
    const actual = Buffer.from(signature, "base64url");
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected)) throw 0;
    payload = decode(claims);
    const now = Date.now() / 1000;
    if (!payload || !Number.isFinite(payload.exp) || payload.exp <= now) throw 0;
    if (payload.nbf !== undefined && (!Number.isFinite(payload.nbf) || payload.nbf > now)) throw 0;
  } catch {
    return error(401, "invalid or expired token");
  }
  const id = typeof payload.sub === "string" ? positiveID(payload.sub) : 0;
  if (!id || typeof payload.username !== "string") return error(401, "invalid token payload");
  return { id, username: payload.username };
}

Bun.serve({
  development: false,
  hostname: HOST,
  port: Number(PORT),
  idleTimeout: 75,
  maxRequestBodySize: 16 * 1024,
  routes: {
    "/health": { GET: () => {
      try {
        health.get();
        return json({ status: "ok", db: "ok", uptime_s: Math.floor((performance.now() - started) / 1000) });
      } catch (e) {
        return json({ status: "degraded", db: "unreachable", error: String(e) }, 503);
      }
    } },
    "/feed": { GET: () => json({ posts: feed.all() }) },
    "/posts/:id": { GET: req => {
      const id = positiveID(req.params.id);
      if (!id) return error(400, "invalid post id");
      const row = post.get(id);
      return row ? json({ post: row }) : error(404, "post not found");
    } },
    "/posts": { POST: async req => {
      const user = authenticate(req);
      if (user instanceof Response) return user;
      let data;
      try { data = await req.json(); }
      catch { return error(400, "malformed JSON body"); }
      if (typeof data?.body !== "string" || !data.body.trim()) return error(400, "body is required");
      const body = data.body.trim();
      if (body.length > 500 && [...body].length > 500) return error(400, "body must be at most 500 characters");
      // all() steps RETURNING to SQLITE_DONE, committing before the response is constructed.
      const row = create.all(user.id, body)[0];
      return json({ post: { id: row.id, body, created_at: row.created_at, author: user.username, like_count: 0 } }, 201);
    } },
    "/posts/:id/like": { POST: req => {
      const user = authenticate(req); // Auth precedes ID validation.
      if (user instanceof Response) return user;
      const id = positiveID(req.params.id);
      if (!id) return error(400, "invalid post id");
      const inserted = like.run(user.id, id).changes === 1;
      if (!inserted && !exists.get(id)) return error(404, "post not found");
      return json({ liked: true, already_liked: !inserted, post_id: id }, inserted ? 201 : 200);
    } },
  },
  fetch: () => error(404, "not found"),
  error: () => error(500, "internal server error"),
});
