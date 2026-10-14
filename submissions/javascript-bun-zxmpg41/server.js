import * as db from "./db.js";
import { verify, MISSING, INVALID } from "./jwt.js";

const started = Date.now();
const HEADERS = { "Content-Type": "application/json" };

const json = (status, body) => new Response(body, { status, headers: HEADERS });
const err = (status, message) => json(status, `{"error":"${message}"}`);

const NOT_FOUND = '{"error":"not found"}';
const ID_RE = /^[1-9]\d*$/;

function parseId(s) {
  if (!ID_RE.test(s)) return 0;
  const n = Number(s);
  return Number.isSafeInteger(n) ? n : 0;
}

function auth(req) {
  const u = verify(req.headers.get("authorization"));
  if (typeof u === "object") return u;
  if (u === MISSING) return err(401, "missing bearer token");
  if (u === INVALID) return err(401, "invalid or expired token");
  return err(401, "invalid token payload");
}

async function createPost(req) {
  const user = auth(req);
  if (user instanceof Response) return user;
  let data;
  try { data = JSON.parse(await req.text()); } catch { return err(400, "malformed JSON body"); }
  let body = data !== null && typeof data === "object" ? data.body : undefined;
  if (typeof body !== "string" || (body = body.trim()).length === 0) return err(400, "body is required");
  // Count code points (not UTF-16 units), matching SQLite's length().
  if (body.length > 500 && (body.length > 1000 || [...body].length > 500))
    return err(400, "body must be at most 500 characters");
  const row = await db.createPost(user.id, body);
  return json(201, JSON.stringify({
    post: { id: row.id, body, created_at: row.created_at, author: user.username, like_count: 0 },
  }));
}

async function like(req, idStr) {
  const user = auth(req);
  if (user instanceof Response) return user;
  const id = parseId(idStr);
  if (!id) return err(400, "invalid post id");
  const already = await db.like(user.id, id);
  if (already === null) return err(404, "post not found");
  return json(already ? 200 : 201, `{"liked":true,"already_liked":${already},"post_id":${id}}`);
}

function route(req) {
  const url = req.url;
  const path = url.slice(url.indexOf("/", url.indexOf("//") + 2));
  const q = path.indexOf("?");
  const p = q === -1 ? path : path.slice(0, q);
  const method = req.method;

  if (p === "/feed" && method === "GET") return json(200, db.feed());
  if (p.startsWith("/posts/")) {
    const rest = p.slice(7);
    if (method === "GET" && rest.indexOf("/") === -1) {
      const id = parseId(rest);
      if (!id) return err(400, "invalid post id");
      const body = db.post(id);
      return body === null ? err(404, "post not found") : json(200, body);
    }
    if (method === "POST" && rest.endsWith("/like") && rest.indexOf("/") === rest.length - 5)
      return like(req, rest.slice(0, -5));
  }
  if (p === "/posts" && method === "POST") return createPost(req);
  if (p === "/health" && method === "GET") {
    try {
      db.health();
      return json(200, `{"status":"ok","db":"ok","uptime_s":${Math.floor((Date.now() - started) / 1000)}}`);
    } catch (e) {
      return json(503, JSON.stringify({ status: "degraded", db: "unreachable", error: String(e?.message ?? e) }));
    }
  }
  return json(404, NOT_FOUND);
}

Bun.serve({
  hostname: process.env.HOST ?? "0.0.0.0",
  port: Number(process.env.PORT ?? 80),
  idleTimeout: 255, // max; spec wants >= 65 s keep-alive
  fetch(req) {
    try {
      const r = route(req);
      return r instanceof Promise ? r.catch(onError) : r;
    } catch (e) {
      return onError(e);
    }
  },
  error: onError,
});

function onError(e) {
  console.error(e);
  return err(500, "internal server error");
}

console.log(`listening on ${process.env.HOST ?? "0.0.0.0"}:${process.env.PORT ?? 80}`);
