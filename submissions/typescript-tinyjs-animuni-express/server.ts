import { Database } from "bun:sqlite";
import { createHmac, timingSafeEqual } from "node:crypto";

const json = Response.json;
const error = (status: number, message: string) => json({ error: message }, { status });
function parseId(value: unknown) {
  if (typeof value !== "string" || !value || /\D/.test(value)) return null;
  const id = Number(value);
  return Number.isSafeInteger(id) && id > 0 ? id : null;
}

function authenticate(request: Request) {
  const authorization = request.headers.get("authorization");
  if (!authorization?.startsWith("Bearer ")) return error(401, "missing bearer token");
  const invalid = () => error(401, "invalid or expired token");
  const token = authorization.slice(7);
  if (!/^[\w-]+\.[\w-]+\.[\w-]+$/.test(token)) return invalid();
  const [header, claims, signature] = token.split(".");
  try {
    const actual = Buffer.from(signature, "base64url");
    const secret = process.env.JWT_SECRET;
    if (!secret) return invalid();
    const expected = createHmac("sha256", secret).update(header + "." + claims).digest();
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected) ||
        actual.toString("base64url") !== signature) return invalid();
    if (JSON.parse(Buffer.from(header, "base64url").toString())?.alg !== "HS256") return invalid();
    const payload = JSON.parse(Buffer.from(claims, "base64url").toString());
    if (typeof payload?.exp !== "number" || !Number.isFinite(payload.exp) ||
        payload.exp <= Date.now() / 1000) return invalid();
    const id = parseId(payload.sub);
    if (id === null || typeof payload.username !== "string") return error(401, "invalid token payload");
    return { id, username: payload.username as string };
  } catch {
    return invalid();
  }
}

function openStore() {
  const path = process.env.SQLITE_PATH;
  if (!path) throw new Error("SQLITE_PATH is required");
  const db = new Database(path, { create: false, strict: true });
  try {
    db.exec("PRAGMA busy_timeout=5000; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON");
    const select = "SELECT p.id, p.body, p.created_at, u.username AS author, " +
      "(SELECT count(*) FROM likes WHERE post_id = p.id) AS like_count " +
      "FROM posts p JOIN users u ON u.id = p.user_id";
    const exists = db.query("SELECT 1 FROM posts WHERE id = ?");
    const insert = db.query("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING");
    return {
      ping: db.query("SELECT 1"),
      feed: db.query(select + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"),
      post: db.query(select + " WHERE p.id = ?"),
      create: db.query<{ id: number; created_at: string }, [number, string]>(
        "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"),
      like: db.transaction((user: number, post: number) =>
        exists.get(post) ? insert.run(user, post).changes > 0 : null).immediate,
    };
  } catch (cause) {
    db.close();
    throw cause;
  }
}

let store: ReturnType<typeof openStore> | undefined;
const database = () => store ??= openStore();
const failed = () => error(500, "internal server error");
type Context = { params: { id: string } };

export function health() {
  try {
    database().ping.get();
    return json({ status: "ok", db: "ok", uptime_s: Math.floor(process.uptime()) });
  } catch {
    return json({ status: "degraded", db: "unreachable", error: "database unavailable" }, { status: 503 });
  }
}

export function feed() {
  try {
    return json({ posts: database().feed.all() });
  } catch {
    return failed();
  }
}

export function getPost(_request: Request, { params }: Context) {
  const id = parseId(params.id);
  if (id === null) return error(400, "invalid post id");
  try {
    const post = database().post.get(id);
    return post ? json({ post }) : error(404, "post not found");
  } catch {
    return failed();
  }
}

export async function createPost(request: Request) {
  const user = authenticate(request);
  if (user instanceof Response) return user;
  let input;
  try {
    input = await request.json();
  } catch {
    return error(400, "malformed JSON body");
  }
  const body = typeof input?.body === "string" ? input.body.trim() : "";
  if (!body) return error(400, "body is required");
  if (Array.from(body).length > 500) return error(400, "body must be at most 500 characters");
  try {
    const row = database().create.get(user.id, body);
    if (!row) return failed();
    return json({ post: { id: row.id, body, created_at: row.created_at, author: user.username, like_count: 0 } },
      { status: 201 });
  } catch {
    return failed();
  }
}

export function likePost(request: Request, { params }: Context) {
  const user = authenticate(request);
  if (user instanceof Response) return user;
  const id = parseId(params.id);
  if (id === null) return error(400, "invalid post id");
  try {
    const inserted = database().like(user.id, id);
    if (inserted === null) return error(404, "post not found");
    return json({ liked: true, already_liked: !inserted, post_id: id }, { status: inserted ? 201 : 200 });
  } catch {
    return failed();
  }
}

export const notFound = () => error(404, "not found");
