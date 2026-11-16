import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { readFile, readdir, mkdtemp, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { Database } from "bun:sqlite";
import { createConnection } from "node:net";
import { Agent, request as httpRequest } from "node:http";
import type { Socket } from "node:net";

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
const sign = (payload: unknown, alg = "HS256", key = secret) => {
  const message = encode({ alg }) + "." + encode(payload);
  return message + "." + createHmac("sha256", key).update(message).digest("base64url");
};

function slowUpload(url: string, stop?: () => void) {
  return new Promise<number>((resolve, reject) => {
    const body = '{"body":"slow upload"}';
    const pending = httpRequest(url + prefix + "/posts", {
      method: "POST", headers: { authorization: "Bearer " + token, "content-length": Buffer.byteLength(body) },
    }, (response) => {
      let result = "";
      response.on("data", (chunk) => { result += chunk.toString(); });
      response.on("end", () => {
        try { assert.equal(response.statusCode, 201); resolve(JSON.parse(result).post.id); } catch (error) { reject(error); }
      });
    });
    const send = setTimeout(() => pending.end(body), 6000);
    const shutdown = setTimeout(() => stop?.(), 1000);
    pending.on("error", (error) => { clearTimeout(send); clearTimeout(shutdown); reject(error); });
    pending.setTimeout(10000, () => pending.destroy(new Error("slow upload timed out")));
    pending.flushHeaders();
  });
}

function agentRequest(url: string, agent: Agent, body?: string) {
  return new Promise<{ status: number; body: any; connection?: string; socket?: Socket }>((resolve, reject) => {
    let socket: Socket | undefined;
    const pending = httpRequest(url + prefix + (body === undefined ? "/health" : "/posts"), {
      agent, method: body === undefined ? "GET" : "POST",
      headers: { authorization: "Bearer " + token },
    }, (response) => {
      let result = "";
      response.on("data", (chunk) => { result += chunk.toString(); });
      response.on("error", reject);
      response.on("end", () => {
        try { resolve({ status: response.statusCode!, body: JSON.parse(result),
          connection: response.headers.connection, socket }); } catch (error) { reject(error); }
      });
    });
    pending.on("socket", (value) => { socket = value; });
    pending.on("error", reject);
    pending.setTimeout(10000, () => pending.destroy(new Error("agent request timed out")));
    pending.end(body);
  });
}

async function withServer(sqlitePath: string, run: (url: string, stop: () => void, pid: number) => Promise<void>,
    host = cli === "transport.ts" ? "0.0.0.0" : "127.0.0.1", fdLimit?: number) {
  const entry = [process.execPath, cli, "serve", "--port", "0"];
  const child = Bun.spawn(fdLimit === undefined ? entry :
    ["bash", "-c", 'ulimit -n "$1"; shift; exec "$@"', "bash", String(fdLimit), ...entry], {
    env: { ...process.env, HOST: host, PORT: "0", SQLITE_PATH: sqlitePath, JWT_SECRET: secret },
    stdout: "pipe",
  });
  let stopping = false;
  const stop = () => {
    if (!stopping) { stopping = true; child.kill("SIGTERM"); }
  };
  try {
    const url = await Promise.race([
      (async () => {
        let output = "";
        for await (const chunk of child.stdout) {
          output += new TextDecoder().decode(chunk);
          const match = /http:\/\/(?:127\.0\.0\.1|0\.0\.0\.0):\d+/.exec(output);
          if (match) return match[0].replace("0.0.0.0", "127.0.0.1");
        }
        throw new Error("TinyJS exited before announcing its listening address");
      })(),
      new Promise<never>((_, reject) =>
        setTimeout(() => reject(new Error("TinyJS startup timed out")), 10000).unref()),
    ]);
    await run(url, stop, child.pid);
  } finally {
    const timeout = setTimeout(() => child.kill("SIGKILL"), 5000);
    stop();
    try {
      assert.equal(await child.exited, 0, "server must shut down gracefully within five seconds");
    } finally {
      clearTimeout(timeout);
    }
  }
}

try {
  let id: number;
  let shutdownId: number | undefined;
  await withServer(path, async (url) => {
    const get = (route: string) => fetch(url + prefix + route);
    const post = (route: string, body?: string, authorization = "Bearer " + token) =>
      fetch(url + prefix + route, { method: "POST", headers: { authorization }, body });
    assert.equal((await get("/health")).status, 200);
    const head = await fetch(url + prefix + "/feed", { method: "HEAD" });
    assert.equal(head.status, 200);
    assert.equal(await head.text(), "");
    assert.equal((await fetch(url + prefix + "/feed", { method: "POST" })).status, 405);
    if (prefix) assert.equal((await fetch(url + "/health")).status, 404);
    assert.deepEqual(await (await get("/")).json(), { error: "not found" });
    assert.deepEqual(await (await get("/unknown")).json(), { error: "not found" });
    assert.equal((await post("/posts/abc/like", undefined, "Basic wrong")).status, 401);
    assert.equal((await post("/posts/abc/like")).status, 400);
    assert.equal((await post("/posts", "{bad json")).status, 400);
    const claims = { sub: "1", username: "alice", exp: Math.floor(Date.now() / 1000) + 600 };
    for (const invalid of [
      sign(claims, "HS256", "wrong-secret"), sign(claims, "HS512"),
      sign({ ...claims, exp: 1 }), sign({ ...claims, sub: 1 }),
      sign({ ...claims, username: null }), token + "=", "garbage",
    ]) {
      const response = await post("/posts", '{"body":"must not commit"}', "Bearer " + invalid);
      assert.equal(response.status, 401);
      await response.arrayBuffer();
    }
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
    const creates = await Promise.all(Array.from({ length: 24 }, (_, index) =>
      post("/posts", JSON.stringify({ body: "concurrent " + index }))));
    const ids = new Set<number>();
    for (const response of creates) {
      assert.equal(response.status, 201);
      ids.add((await response.json()).post.id);
    }
    assert.equal(ids.size, 24);
    const afterCreates = await (await get("/feed")).json();
    assert.equal(afterCreates.posts.length, 20);
    assert.ok(afterCreates.posts.every((post: { id: number }) => ids.has(post.id)));
    if (cli === "transport.ts") {
      await slowUpload(url);
      const agent = new Agent({ keepAlive: true, maxSockets: 1 });
      try {
        const first = await agentRequest(url, agent);
        for (const gap of [8000, 16000]) {
          await Bun.sleep(gap);
          const response = await agentRequest(url, agent, JSON.stringify({ body: "after idle " + gap }));
          assert.equal(response.status, 201);
          assert.equal(response.socket, first.socket, "direct idle connection must remain reusable for POST");
          assert.notEqual(response.connection, "close");
          const created = response.body.post;
          assert.equal((await (await get("/posts/" + created.id)).json()).post.body, created.body);
        }
      } finally {
        agent.destroy();
      }
    }
  });
  if (cli === "transport.ts") {
    if (process.platform === "linux") {
      await withServer(path, async (url, _stop, pid) => {
        const idle: Socket[] = [];
        const agent = new Agent({ keepAlive: true, maxSockets: 1 });
        try {
          let observedRetirement = false;
          while (!observedRetirement && (await readdir(`/proc/${pid}/fd`)).length < Math.ceil(128 * 0.75)) {
            assert.ok(idle.length < 128, "pressure fixture must reach its descriptor threshold");
            await new Promise<void>((resolve, reject) => {
              const socket = createConnection(Number(new URL(url).port), "127.0.0.1");
              idle.push(socket);
              socket.setTimeout(10000, () => socket.destroy(new Error("pressure fixture timed out")));
              socket.on("error", reject);
              socket.on("connect", () => socket.write("GET " + prefix + "/health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"));
              socket.once("data", (chunk) => {
                try {
                  assert.match(chunk.toString(), /^HTTP\/1\.1 200/);
                  observedRetirement = /\r\nconnection:\s*close\r\n/i.test(chunk.toString());
                  resolve();
                } catch (error) { reject(error); }
              });
            });
          }
          await Bun.sleep(1300);
          let previous: Socket | undefined;
          const ids = new Set<number>();
          for (let index = 0; index < 4; index++) {
            const result = await agentRequest(url, agent, JSON.stringify({ body: "retired " + index }));
            assert.equal(result.status, 201);
            assert.equal(result.connection, "close", "retirement must be advertised on the response");
            assert.notEqual(result.socket, previous, "the next write must reconnect without replay");
            ids.add(result.body.post.id);
            previous = result.socket;
          }
          assert.equal(ids.size, 4);
          idle.forEach((socket) => socket.destroy());
          await Bun.sleep(1300);
          const recovered = await agentRequest(url, agent);
          assert.equal(recovered.status, 200);
          assert.notEqual(recovered.connection, "close", "normal keep-alive must resume after pressure falls");
        } finally {
          idle.forEach((socket) => socket.destroy());
          agent.destroy();
        }
      }, "0.0.0.0", 128);
    }
    await withServer(path, async (url, stop) => { shutdownId = await slowUpload(url, stop); });
    await withServer(path, async (url) => {
      await new Promise<void>((resolve, reject) => {
        const socket = createConnection(Number(new URL(url).port), "127.0.0.1");
        let responses = "";
        const request = "GET " + prefix + "/health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        const reuse = setTimeout(() => socket.write(request), 6000);
        socket.setTimeout(10000, () => socket.destroy(new Error("proxy keep-alive timed out")));
        socket.on("connect", () => socket.write(request));
        socket.on("data", (chunk) => {
          responses += chunk.toString();
          if ((responses.match(/HTTP\/1\.1 200/g) ?? []).length === 2) { socket.destroy(); resolve(); }
        });
        socket.on("end", () => reject(new Error("proxy socket closed before reuse")));
        socket.on("error", reject);
        socket.on("close", () => clearTimeout(reuse));
      });
    }, "127.0.0.1");
  }
  await withServer(path, async (url) => {
    const response = await fetch(url + prefix + "/posts/" + id);
    assert.equal(response.status, 200);
    assert.equal((await response.json()).post.like_count, 1);
    if (shutdownId !== undefined) {
      const post = (await (await fetch(url + prefix + "/posts/" + shutdownId)).json()).post;
      assert.equal(post.body, "slow upload");
      assert.equal(post.like_count, 0);
    }
  });
  await withServer(join(root, "missing.db"), async (url) => {
    const response = await fetch(url + prefix + "/health");
    assert.equal(response.status, 503);
    assert.equal((await response.json()).status, "degraded");
  });
  const persisted = new Database(path, { readonly: true });
  try {
    assert.equal(persisted.query("SELECT count(*) AS n FROM posts WHERE body LIKE 'retired %'").get().n,
      cli === "transport.ts" && process.platform === "linux" ? 4 : 0, "retired connections must commit each write exactly once");
    assert.equal(persisted.query("SELECT count(*) AS n FROM posts WHERE body = 'must not commit'").get().n, 0);
  } finally {
    persisted.close();
  }
  console.log("Production smoke passed: invalid auth, malformed JSON, HTTP semantics, concurrent likes/posts, retained idle writes, descriptor-pressure retirement (Linux/native), restart persistence, degraded health.");
} finally {
  await rm(root, { recursive: true, force: true });
}
