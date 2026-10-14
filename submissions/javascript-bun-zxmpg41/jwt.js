// Hand-rolled HS256 JWT verification. Every token is verified on every request (no cache).
import { createHmac, timingSafeEqual } from "node:crypto";

const SECRET = process.env.JWT_SECRET ?? "";

export const MISSING = 1;
export const INVALID = 2;
export const PAYLOAD = 3;

// Returns { id, username } or an error code (MISSING | INVALID | PAYLOAD).
export function verify(authHeader) {
  if (!authHeader || !authHeader.startsWith("Bearer ")) return MISSING;
  const token = authHeader.slice(7);
  const d1 = token.indexOf(".");
  const d2 = token.indexOf(".", d1 + 1);
  if (d1 <= 0 || d2 < 0 || token.indexOf(".", d2 + 1) !== -1) return INVALID;

  let header, payload;
  try {
    header = JSON.parse(Buffer.from(token.slice(0, d1), "base64url").toString());
    if (header === null || typeof header !== "object" || header.alg !== "HS256") return INVALID;
    const sig = Buffer.from(token.slice(d2 + 1), "base64url");
    const expected = createHmac("sha256", SECRET).update(token.slice(0, d2)).digest();
    if (sig.length !== expected.length || !timingSafeEqual(sig, expected)) return INVALID;
    payload = JSON.parse(Buffer.from(token.slice(d1 + 1, d2), "base64url").toString());
  } catch {
    return INVALID;
  }
  if (payload === null || typeof payload !== "object") return INVALID;
  if (typeof payload.exp !== "number" || payload.exp * 1000 <= Date.now()) return INVALID;

  const sub = payload.sub;
  if (typeof sub !== "string" || !/^[1-9]\d*$/.test(sub)) return PAYLOAD;
  const id = Number(sub);
  if (!Number.isSafeInteger(id)) return PAYLOAD;
  if (typeof payload.username !== "string") return PAYLOAD;
  return { id, username: payload.username };
}
