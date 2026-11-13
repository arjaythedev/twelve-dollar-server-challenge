import { describe, expect, test } from "bun:test";
import { createHmac } from "node:crypto";
import { verifyBearerToken } from "../src/auth";

const SECRET = Buffer.from("twelve-dollar-challenge", "utf8");

function b64url(input: string): string {
  return Buffer.from(input, "utf8").toString("base64url");
}

function sign(payload: Record<string, unknown>): string {
  const h = b64url(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const p = b64url(JSON.stringify(payload));
  const sig = createHmac("sha256", SECRET).update(`${h}.${p}`).digest("base64url");
  return `${h}.${p}.${sig}`;
}

describe("verifyBearerToken", () => {
  test("accepts a valid token", () => {
    const now = Math.floor(Date.now() / 1000);
    const token = sign({ sub: "1", username: "alice", iat: now, exp: now + 3600 });
    expect(verifyBearerToken(`Bearer ${token}`, SECRET)).toEqual({ userId: 1, username: "alice" });
  });

  test("rejects missing header before anything else", () => {
    expect(() => verifyBearerToken(null, SECRET)).toThrow("missing bearer token");
  });

  test("rejects bad signature and expiry", () => {
    const now = Math.floor(Date.now() / 1000);
    const good = sign({ sub: "1", username: "alice", iat: now, exp: now + 3600 });
    expect(() => verifyBearerToken(`Bearer ${good.slice(0, -2)}xx`, SECRET)).toThrow(
      "invalid or expired token",
    );
    const expired = sign({ sub: "1", username: "alice", iat: now - 7200, exp: now - 3600 });
    expect(() => verifyBearerToken(`Bearer ${expired}`, SECRET)).toThrow("invalid or expired token");
  });

  test("rejects invalid payload shape", () => {
    const now = Math.floor(Date.now() / 1000);
    const badSub = sign({ sub: "abc", username: "alice", iat: now, exp: now + 3600 });
    expect(() => verifyBearerToken(`Bearer ${badSub}`, SECRET)).toThrow("invalid token payload");
  });
});
