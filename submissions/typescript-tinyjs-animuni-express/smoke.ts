import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { readFile, mkdtemp, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { Database } from "bun:sqlite";

const cli = process.argv[2] ?? "node_modules/tinyjs-fullstack/bin/tinyjs.js";
const prefix = process.argv[3] ?? "";
const root = await mkdtemp(join(tmpdir(), "tinyjs-challenge-"));
const path = join(root, "feed.db");
const secret = "local-smoke-secret";
const db = new Database(path, { create: true });
db.exec(await readFile("../../schema.sql", "utf8"));
db.exec("INSERT INTO users (id, username) VALUES (1, 'alice')");
db.close();
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString("base64url");
const message = encode({ alg: "HS256" }) + "." +
  encode({ sub: "1", username: "alice", exp: Math.floor(Date.now() / 1000) + 600 });
const token = message + "." + createHmac("sha256", secret).update(message).digest("base64url");

async function withServer(sqlitePath: string, run: (url: string) => Promise<void>) {
  const child = Bun.spawn([process.execPath, cli, "serve", "--port", "0"], {
    env: { ...process.env, SQLITE_PATH: sqlitePath, JWT_SECRET: secret },
    stdout: "pipe",
  });
  try {
    const reader = child.stdout.getReader();
    const { value } = await reader.read();
    const url = /http:\/\/127\.0\.0\.1:\d+/.exec(new TextDecoder().decode(value))?.[0];
    assert.ok(url, "TinyJS did not announce its listening address");
    await run(url);
  } finally {
    child.kill();
    await child.exited;
  }
}

try {
  let id: number;
  await withServer(path, async (url) => {
    const get = (route: string) => fetch(url + prefix + route);
    const post = (route: string, body?: string, authorization = "Bearer " + token) =>
      fetch(url + prefix + route, { method: "POST", headers: { authorization }, body });
    assert.equal((await get("/health")).status, 200);
    if (prefix) assert.equal((await fetch(url + "/health")).status, 404);
    assert.deepEqual(await (await get("/")).json(), { error: "not found" });
    assert.deepEqual(await (await get("/unknown")).json(), { error: "not found" });
    assert.equal((await post("/posts/abc/like", undefined, "Basic wrong")).status, 401);
    assert.equal((await post("/posts/abc/like")).status, 400);
    assert.equal((await post("/posts", "{bad json")).status, 400);
    const created = await post("/posts", JSON.stringify({ body: " hello " }));
    assert.equal(created.status, 201);
    const result = await created.json();
    id = result.post.id;
    assert.deepEqual(Object.keys(result.post), ["id", "body", "created_at", "author", "like_count"]);
    assert.equal(result.post.body, "hello");
    const likes = await Promise.all(Array.from({ length: 12 }, () => post("/posts/" + id + "/like")));
    assert.equal(likes.filter((response) => response.status === 201).length, 1);
    assert.equal(likes.filter((response) => response.status === 200).length, 11);
    await Promise.all(likes.map((response) => response.json()));
    assert.equal((await (await get("/feed")).json()).posts[0].like_count, 1);
  });
  await withServer(path, async (url) => {
    const response = await fetch(url + prefix + "/posts/" + id);
    assert.equal(response.status, 200);
    assert.equal((await response.json()).post.like_count, 1);
  });
  await withServer(join(root, "missing.db"), async (url) => {
    const response = await fetch(url + prefix + "/health");
    assert.equal(response.status, 503);
    assert.equal((await response.json()).status, "degraded");
  });
  console.log("Production smoke passed: auth order, malformed JSON, response shape, concurrent likes, restart persistence, degraded health.");
} finally {
  await rm(root, { recursive: true, force: true });
}
