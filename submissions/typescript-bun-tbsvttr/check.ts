// Extra integration checks; run after test/test.sh, with no concurrent load test.
// JWT_SECRET=... bun check.ts [http://127.0.0.1:3000]
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";

const base = (process.argv[2] || "http://127.0.0.1:3000").replace(/\/$/, "");
const secret = process.env.JWT_SECRET || "twelve-dollar-challenge";
const now = () => Math.floor(Date.now() / 1000);
const claims = { sub: process.env.TEST_USER_ID || "1", username: "integration ✓", exp: now() + 3600 };
const header = { alg: "HS256", typ: "JWT" };
const encode = (value: unknown) => Buffer.from(JSON.stringify(value));
function sign(h: Buffer, p: Buffer) {
  const text = `${h.toString("base64url")}.${p.toString("base64url")}`;
  return `${text}.${createHmac("sha256", secret).update(text).digest("base64url")}`;
}
const token = (p: unknown = claims, h: unknown = header) => sign(encode(h), encode(p));
const auth = token();
async function request(method: string, path: string, jwt?: string, body?: unknown) {
  const response = await fetch(base + path, {
    method, headers: jwt ? { Authorization: `Bearer ${jwt}`, "Content-Type": "application/json" } : {},
    body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(5000),
  });
  return { status: response.status, body: await response.json() as any };
}
const ok = (label: string) => console.log(`  ok   ${label}`);
async function reject(jwt: string, message = "invalid or expired token") {
  assert.deepEqual(await request("POST", "/posts", jwt, { body: "must not be inserted" }),
    { status: 401, body: { error: message } });
}

const body = "😀".repeat(500);
const created = await request("POST", "/posts", auth, { body: ` \n${body}\t ` });
assert.equal(created.status, 201);
assert.equal(created.body.post.body, body);
assert.equal(created.body.post.author, claims.username);
const id = created.body.post.id;
assert.ok(Number.isSafeInteger(id) && id > 0);
assert.deepEqual(await request("POST", "/posts", auth, { body: "😀".repeat(501) }),
  { status: 400, body: { error: "body must be at most 500 characters" } });
const before = await request("GET", `/posts/${id}`);
assert.equal(before.status, 200);
assert.equal(before.body.post.body, body);
assert.equal(before.body.post.like_count, 0);
ok("500 Unicode code points round-trip; 501 are rejected after trimming");

const duplicates = await Promise.all(Array.from({ length: 16 }, () => request("POST", `/posts/${id}/like`, auth)));
assert.equal(duplicates.filter(r => r.status === 201).length, 1);
for (const r of duplicates) {
  assert.ok(r.status === 200 || r.status === 201);
  assert.deepEqual(r.body, { liked: true, already_liked: r.status === 200, post_id: id });
}
const after = await request("GET", `/posts/${id}`);
assert.equal(after.status, 200);
assert.equal(after.body.post.like_count, 1);
const refreshed = await request("GET", "/feed");
assert.equal(refreshed.status, 200);
assert.equal(refreshed.body.posts.find((p: any) => p.id === id)?.like_count, 1);
ok("16 concurrent identical likes insert exactly one row; post and feed reads update");

await reject(token(claims, { alg: "HS512", typ: "JWT" }));
await reject(token(claims, null));
await reject(token({ ...claims, exp: String(claims.exp) }));
await reject(token({ ...claims, nbf: now() + 3600 }));
await reject(token({ ...claims, sub: 1 }), "invalid token payload");
await reject(token({ ...claims, username: null }), "invalid token payload");
await reject(sign(encode(header), Buffer.from("{")));
await reject(sign(encode(header), Buffer.concat([
  Buffer.from('{"sub":"1","username":"'), Buffer.from([0xc3, 0x28]), Buffer.from(`","exp":${claims.exp}}`),
])));
await reject(sign(Buffer.concat([
  Buffer.from('{"alg":"HS256","x":"'), Buffer.from([0xc3, 0x28]), Buffer.from('"}'),
]), encode(claims)));
ok("signed invalid algorithms, claims, JSON and UTF-8 JWT segments are rejected");

const expiry = now() + 2;
const shortLived = token({ ...claims, exp: expiry });
assert.equal((await request("POST", `/posts/${id}/like`, shortLived)).status, 200);
await Bun.sleep(Math.max(0, expiry * 1000 - Date.now() + 100));
assert.deepEqual(await request("POST", `/posts/${id}/like`, shortLived),
  { status: 401, body: { error: "invalid or expired token" } });
ok("the same previously accepted JWT is rejected after expiry");

const burst = await Promise.all(Array.from({ length: 12 }, (_, i) =>
  request("POST", "/posts", auth, { body: `ordering ${i}: ${crypto.randomUUID()}` })));
for (const r of burst) assert.equal(r.status, 201);
const expected = burst.map(r => r.body.post).sort((a, b) =>
  a.created_at < b.created_at ? 1 : a.created_at > b.created_at ? -1 : b.id - a.id);
const latest = await request("GET", "/feed");
assert.equal(latest.status, 200);
assert.deepEqual(latest.body.posts.slice(0, expected.length).map((p: any) => p.id), expected.map(p => p.id));
const ties = expected.slice(1).filter((p, i) => p.created_at === expected[i].created_at).length;
ok(`feed immediately reflects concurrent creates in timestamp/id order (${ties} timestamp ties)`);
console.log(`Extra checks passed. Post ${id} has 500 emoji and exactly one like for optional restart verification.`);
