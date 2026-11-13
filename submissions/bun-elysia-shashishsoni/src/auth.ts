import { createHmac, timingSafeEqual } from "node:crypto";
import { invalidToken, invalidTokenPayload, missingBearer } from "./errors";

export interface AuthenticatedUser {
  userId: number;
  username: string;
}

function decodeBase64UrlToJson(segment: string): unknown {
  const buf = Buffer.from(segment, "base64url");
  return JSON.parse(buf.toString("utf8"));
}

function verifySignature(signingInput: string, signatureB64: string, secret: Buffer): boolean {
  const digest = createHmac("sha256", secret).update(signingInput).digest();
  let provided: Buffer;
  try {
    provided = Buffer.from(signatureB64, "base64url");
  } catch {
    return false;
  }
  if (provided.length !== digest.length) return false;
  return timingSafeEqual(digest, provided);
}

function isPositiveIntString(value: unknown): value is string {
  if (typeof value !== "string" || value.length === 0) return false;
  let nonzero = false;
  for (let i = 0; i < value.length; i++) {
    const code = value.charCodeAt(i);
    if (code < 48 || code > 57) return false;
    if (code !== 48) nonzero = true;
  }
  return nonzero;
}

// b64url('{"alg":"HS256","typ":"JWT"}'): what seed/make-seed.sh and test/test.sh sign with.
// Matching headers skip a base64 decode + JSON.parse on every authed request.
const EXPECTED_HEAD_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

export function verifyBearerToken(
  authorization: string | null,
  secret: Buffer,
  nowSec: number = Math.floor(Date.now() / 1000),
): AuthenticatedUser {
  if (!authorization || !authorization.startsWith("Bearer ")) {
    throw missingBearer();
  }
  const token = authorization.slice(7);
  const dot1 = token.indexOf(".");
  const dot2 = token.indexOf(".", dot1 + 1);
  if (dot1 < 1 || dot2 < 0 || token.indexOf(".", dot2 + 1) !== -1) {
    throw invalidToken();
  }
  const headB64 = token.slice(0, dot1);
  const payloadB64 = token.slice(dot1 + 1, dot2);
  const sigB64 = token.slice(dot2 + 1);

  let payload: unknown;
  if (headB64 !== EXPECTED_HEAD_B64) {
    let header: unknown;
    try {
      header = decodeBase64UrlToJson(headB64);
    } catch {
      throw invalidToken();
    }
    if (typeof header !== "object" || header === null) throw invalidToken();
    if ((header as Record<string, unknown>).alg !== "HS256") throw invalidToken();
  }
  try {
    payload = decodeBase64UrlToJson(payloadB64);
  } catch {
    throw invalidToken();
  }
  if (!verifySignature(`${headB64}.${payloadB64}`, sigB64, secret)) {
    throw invalidToken();
  }
  if (typeof payload !== "object" || payload === null) throw invalidToken();

  const claims = payload as Record<string, unknown>;
  const exp = claims.exp;
  if (exp !== undefined) {
    if (typeof exp === "boolean" || (typeof exp !== "number")) throw invalidToken();
    if (nowSec >= exp) throw invalidToken();
  }
  const nbf = claims.nbf;
  if (nbf !== undefined) {
    if (typeof nbf === "boolean" || typeof nbf !== "number") throw invalidToken();
    if (nowSec < nbf) throw invalidToken();
  }

  const sub = claims.sub;
  const username = claims.username;
  if (!isPositiveIntString(sub) || typeof username !== "string") {
    throw invalidTokenPayload();
  }
  return { userId: Number(sub), username };
}
