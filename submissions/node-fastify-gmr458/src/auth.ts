import type { FastifyReply, FastifyRequest } from "fastify";

import * as crypto from "node:crypto";

import { JWT_SECRET } from "./config.js";
import { parseId, MAX_ID } from "./ids.js";
import { sendError } from "./responses.js";

export type AuthResult =
  | { kind: "ok"; userId: number; username: string }
  | { kind: "missing" | "invalid" | "payload" };

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

const STD_PREFIX = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.";

function headerOk(segment: string): boolean {
  try {
    const head: unknown = JSON.parse(Buffer.from(segment, "base64url").toString("utf8"));
    return isRecord(head) && head["alg"] === "HS256";
  } catch {
    return false;
  }
}

export function authenticate(request: FastifyRequest): AuthResult {
  const header = request.headers.authorization;
  if (typeof header !== "string" || !header.startsWith("Bearer ")) return { kind: "missing" };
  const dot2 = header.lastIndexOf(".");
  let dot1: number;
  if (header.startsWith(STD_PREFIX)) {
    dot1 = STD_PREFIX.length - 1;
  } else {
    dot1 = header.indexOf(".", 7);
    if (dot1 < 0 || dot1 >= dot2 || !headerOk(header.slice(7, dot1))) return { kind: "invalid" };
  }
  if (dot2 <= dot1) return { kind: "invalid" };
  const sig = crypto
    .createHmac("sha256", JWT_SECRET)
    .update(header.slice(7, dot2), "utf8")
    .digest("base64url");
  if (header.length - dot2 - 1 !== sig.length) return { kind: "invalid" };
  let diff = 0;
  for (let i = 0; i < sig.length; i++) {
    diff |= sig.charCodeAt(i) ^ header.charCodeAt(dot2 + 1 + i);
  }
  if (diff !== 0) return { kind: "invalid" };

  let payload: unknown;
  try {
    payload = JSON.parse(Buffer.from(header.slice(dot1 + 1, dot2), "base64url").toString("utf8"));
  } catch {
    return { kind: "invalid" };
  }
  if (!isRecord(payload)) return { kind: "payload" };
  const exp: unknown = payload["exp"];
  const nbf: unknown = payload["nbf"];
  if (exp !== undefined || nbf !== undefined) {
    const now = Math.floor(Date.now() / 1000);
    if (exp !== undefined && (typeof exp !== "number" || !Number.isFinite(exp) || now >= exp)) {
      return { kind: "invalid" };
    }
    if (nbf !== undefined && (typeof nbf !== "number" || !Number.isFinite(nbf) || now < nbf)) {
      return { kind: "invalid" };
    }
  }
  const sub: unknown = payload["sub"];
  const username: unknown = payload["username"];
  const userId = typeof sub === "string" ? parseId(sub) : null;
  if (userId === null || userId > MAX_ID || typeof username !== "string") {
    return { kind: "payload" };
  }
  return { kind: "ok", userId, username };
}

export function sendAuthError(reply: FastifyReply, auth: AuthResult): void {
  if (auth.kind === "missing") sendError(reply, 401, "missing bearer token");
  else if (auth.kind === "payload") sendError(reply, 401, "invalid token payload");
  else if (auth.kind === "invalid") sendError(reply, 401, "invalid or expired token");
  else sendError(reply, 500, "internal server error");
}
