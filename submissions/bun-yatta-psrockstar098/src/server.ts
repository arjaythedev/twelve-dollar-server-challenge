/**
 * bun-yatta-psrockstar098 — Yatta JS entry for the $12 Server Challenge.
 *
 * A 5-endpoint feed API (SPEC.md) served by Yatta JS's published modules:
 *   - HTTP routing: `createAPI` / `API` from `yatta.js/api` (src/types/api.ts)
 *   - SQLite: `createDatabase` from `yatta.js/db` (src/types/db.ts), which opens
 *     the file at $SQLITE_PATH with bun:sqlite and applies WAL + synchronous
 *     NORMAL + busy_timeout pragmas. No schema is passed, so Yatta never runs
 *     its schema sync against the challenge database — the fixed schema is
 *     untouched. All queries go through Yatta's rawAll/rawGet/rawRun helpers,
 *     which reuse prepared statements (explicitly allowed by rule 5).
 *   - Serving: Bun.serve with fetch -> api.handle. HTTP/1.1 keep-alive is
 *     Bun's default. idleTimeout is set to 120 s: the challenge requires idle
 *     connections to stay open at least 65 s when behind nginx, and the load
 *     test's think/idle gaps run to ~15 s per user in direct mode.
 *   - Auth: HS256 JWT verification is done in this file with
 *     Bun.CryptoHasher (logic adapted from yatta.js/jwt; the entry keeps
 *     npm yatta.js@1.0.1, so the verifier is inlined here until a
 *     published Yatta version carries the export). Yatta's auth subsystem
 *     is a full accounts system whose error strings do not match SPEC.md,
 *     so it is not used here. Every request verifies the signature and
 *     exp; nothing about a token is ever cached.
 *
 * Nothing is cached across requests: every read runs its SQL while being
 * served, every write is committed (WAL + synchronous NORMAL) before the
 * response is sent, and no response is ever hard-coded.
 */

import { createAPI } from "yatta.js/api";
import { createDatabase } from "yatta.js/db";
import { timingSafeEqual } from "node:crypto";

const SQLITE_PATH = process.env.SQLITE_PATH ?? "";
const JWT_SECRET = process.env.JWT_SECRET ?? "";
const HOST = process.env.HOST ?? "127.0.0.1";
const PORT = Number(process.env.PORT ?? 3000);

if (!SQLITE_PATH) {
  console.error("SQLITE_PATH is not set");
  process.exit(1);
}
if (!JWT_SECRET) {
  console.error("JWT_SECRET is not set");
  process.exit(1);
}

// Open the challenge database through Yatta's DB layer. No `schema` option
// is given, so no migration/sync runs against it; the file is used as-is.
const db = createDatabase({ path: SQLITE_PATH });

const startedAt = Date.now();

// ---------------------------------------------------------------------------
// Response helpers (exact bytes per SPEC.md: compact JSON, key order as
// listed in the spec, Content-Type application/json; charset=utf-8 via the
// local json() helper).
// ---------------------------------------------------------------------------

// Local JSON responder: builds the Response with a plain-object headers
// literal (no `new Headers()` allocation) and the exact bytes SPEC.md
// requires — compact JSON, `application/json; charset=utf-8`. Same bytes
// as Yatta's API.json (the round-3 framework fast path does the same);
// verified by test/test.sh golden checks. `API.json` is not used here so
// the entry pays no Headers allocation per response.
const json = (data: unknown, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });

const err = (message: string, status: number) =>
  json({ error: message }, status);

type PostRow = {
  id: number;
  body: string;
  created_at: string;
  author: string;
  like_count: number;
};

const POST_SELECT = `
  SELECT p.id, p.body, p.created_at, u.username AS author,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id
`;
// Rows already carry the exact wire keys in the exact wire order
// (id, body, created_at, author, like_count), so the feed and single-post
// handlers send them straight to JSON with no per-row mapping step.

// ---------------------------------------------------------------------------
// JWT verification (HS256, secret $JWT_SECRET). Returns the authed user, or
// the exact SPEC.md error string for the failure. Auth is checked before
// any id validation on routes that take both (SPEC.md ordering rule).
// ---------------------------------------------------------------------------

type AuthOk = { ok: true; userId: number; username: string };
type AuthFail = { ok: false; error: string };
type AuthResult = AuthOk | AuthFail;

function verifyAuth(req: Request): AuthResult {
  const header = req.headers.get("authorization");
  if (!header || !header.startsWith("Bearer ")) {
    return { ok: false, error: "missing bearer token" };
  }
  const token = header.slice("Bearer ".length);
  // Split without allocating a parts array; exactly 3 segments required.
  const i1 = token.indexOf(".");
  const i2 = i1 < 0 ? -1 : token.indexOf(".", i1 + 1);
  if (
    i1 < 0 ||
    i2 < 0 ||
    token.indexOf(".", i2 + 1) >= 0
  ) {
    return { ok: false, error: "invalid or expired token" };
  }
  const headerB64 = token.slice(0, i1);
  const payloadB64 = token.slice(i1 + 1, i2);
  const signatureB64 = token.slice(i2 + 1);

  // Bun/Node decode base64url natively — no manual char replace + padding.
  // The header parses first (the alg gate needs it); the payload parses
  // only AFTER the signature is known good, so a forged token never pays
  // for a JSON parse. Both orders yield identical error strings: every
  // failure before the payload-shape check reports "invalid or expired
  // token", and "invalid token payload" is reachable only after the
  // signature has passed and the payload parsed as an object.
  let headerObj: unknown;
  try {
    headerObj = JSON.parse(
      Buffer.from(headerB64, "base64url").toString("utf8"),
    );
  } catch {
    return { ok: false, error: "invalid or expired token" };
  }
  if (
    typeof headerObj !== "object" ||
    headerObj === null ||
    (headerObj as { alg?: unknown }).alg !== "HS256"
  ) {
    return { ok: false, error: "invalid or expired token" };
  }

  // Signature first (a forged token must not reach payload checks).
  // HMAC-SHA256 via Bun.CryptoHasher: ~2x faster than node:createHmac
  // on this workload (729ns vs 1,423ns per call, Bun 1.4.4). Logic
  // adapted from yatta.js/jwt (Yatta's fast verifier, same MIT project);
  // the entry keeps npm yatta.js@1.0.1, so the verifier is inlined here
  // until a published Yatta version carries the export. Compare digest
  // bytes directly instead of re-encoding to a string: two base64url
  // spellings can decode to the same bytes, and a string comparison
  // would wrongly reject those.
  const digest = new Bun.CryptoHasher("sha256", JWT_SECRET)
    .update(`${headerB64}.${payloadB64}`)
    .digest();
  let givenSig: Buffer;
  try {
    givenSig = Buffer.from(signatureB64, "base64url");
  } catch {
    return { ok: false, error: "invalid or expired token" };
  }
  if (
    digest.length !== givenSig.length ||
    !timingSafeEqual(digest, givenSig)
  ) {
    return { ok: false, error: "invalid or expired token" };
  }

  let payloadObj: unknown;
  try {
    payloadObj = JSON.parse(
      Buffer.from(payloadB64, "base64url").toString("utf8"),
    );
  } catch {
    return { ok: false, error: "invalid or expired token" };
  }
  if (typeof payloadObj !== "object" || payloadObj === null) {
    return { ok: false, error: "invalid or expired token" };
  }
  const payload = payloadObj as { sub?: unknown; username?: unknown; exp?: unknown };
  if (
    typeof payload.sub !== "string" ||
    !/^[1-9][0-9]*$/.test(payload.sub) ||
    typeof payload.username !== "string"
  ) {
    return { ok: false, error: "invalid token payload" };
  }
  const nowSeconds = Math.floor(Date.now() / 1000);
  if (typeof payload.exp !== "number" || payload.exp <= nowSeconds) {
    return { ok: false, error: "invalid or expired token" };
  }
  return {
    ok: true,
    userId: Number(payload.sub),
    username: payload.username,
  };
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

const api = createAPI();

api.get("/health", () => {
  try {
    db.rawGet("SELECT 1 AS one");
  } catch (e) {
    return json(
      {
        status: "degraded",
        db: "unreachable",
        error: e instanceof Error ? e.message : String(e),
      },
      503,
    );
  }
  return json({
    status: "ok",
    db: "ok",
    uptime_s: Math.floor((Date.now() - startedAt) / 1000),
  });
});

api.get("/feed", () => {
  const rows = db.rawAll<PostRow>(
    `${POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`,
  );
  return json({ posts: rows });
});

function validPostId(raw: string): number | null {
  if (!/^[0-9]+$/.test(raw)) return null;
  const id = Number(raw);
  if (id <= 0) return null;
  return id;
}

api.get("/posts/:id", (ctx) => {
  const id = validPostId(ctx.params.id);
  if (id === null) return err("invalid post id", 400);
  const row = db.rawGet<PostRow>(`${POST_SELECT} WHERE p.id = ?`, [id]);
  if (!row) return err("post not found", 404);
  return json({ post: row });
});

api.post("/posts", async (ctx) => {
  const auth = verifyAuth(ctx.req);
  if (!auth.ok) return err(auth.error, 401);

  let parsed: unknown;
  try {
    parsed = JSON.parse(await ctx.req.text());
  } catch {
    return err("malformed JSON body", 400);
  }
  const bodyValue =
    typeof parsed === "object" && parsed !== null && !Array.isArray(parsed)
      ? (parsed as { body?: unknown }).body
      : undefined;
  if (typeof bodyValue !== "string") return err("body is required", 400);
  const trimmed = bodyValue.trim();
  if (trimmed.length === 0) return err("body is required", 400);
  // Code points, not UTF-16 units: a string with .length <= 500 can never
  // hold more than 500 code points, so the spread is skipped unless the
  // string is already over 500 units long. Same verdict as [...trimmed].
  if (trimmed.length > 500 ? [...trimmed].length > 500 : false) {
    return err("body must be at most 500 characters", 400);
  }

  // INSERT ... RETURNING gives the stored created_at (column default) back.
  const row = db.rawGet<{ id: number; created_at: string }>(
    "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
    [auth.userId, trimmed],
  );
  if (!row) return err("internal server error", 500);
  return json(
    {
      post: {
        id: row.id,
        body: trimmed,
        created_at: row.created_at,
        author: auth.username,
        like_count: 0,
      },
    },
    201,
  );
});

api.post("/posts/:id/like", (ctx) => {
  // Auth before id validation (SPEC.md ordering rule).
  const auth = verifyAuth(ctx.req);
  if (!auth.ok) return err(auth.error, 401);
  const id = validPostId(ctx.params.id);
  if (id === null) return err("invalid post id", 400);
  const exists = db.rawGet<{ id: number }>("SELECT id FROM posts WHERE id = ?", [
    id,
  ]);
  if (!exists) return err("post not found", 404);
  const result = db.rawRun(
    "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
    [auth.userId, id],
  );
  const first = result.changes === 1;
  return json(
    { liked: true, already_liked: !first, post_id: id },
    first ? 201 : 200,
  );
});

// ---------------------------------------------------------------------------
// Serve. Yatta's router spells its default 404 `{"error":"Not Found"}`;
// SPEC.md requires lowercase `not found`. Only that exact default body is
// rewritten — handler 404s (`post not found`) pass through untouched.
// ---------------------------------------------------------------------------

const server = Bun.serve({
  hostname: HOST,
  port: PORT,
  idleTimeout: 120,
  async fetch(req) {
    let res: Response;
    try {
      res = await api.handle(req);
    } catch (e) {
      console.error("unhandled handler error:", e);
      return err("internal server error", 500);
    }
    if (res.status === 404) {
      const text = await res.text();
      if (text === '{"error":"Not Found"}') {
        return err("not found", 404);
      }
      return new Response(text, { status: 404, headers: res.headers });
    }
    return res;
  },
});

console.error(
  `bun-yatta server listening on http://${server.hostname}:${server.port} (db: ${SQLITE_PATH})`,
);
