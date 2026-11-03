// PGO training run for build.sh: drives server.ts's handle() through the k6 mix (feed-heavy, some
// likes and posts) on a synthetic database with the challenge schema. The seed is not needed at build.
// Usage: deno run --allow-ffi --allow-env --allow-read --allow-write bench/pgo.ts
import { Buffer } from "node:buffer";
import { createHmac } from "node:crypto";
import { Db } from "../sqlite.ts";

const SECRET = "pgo-training";
const dir = await Deno.makeTempDir();
const setup = new Db(`${dir}/pgo.db`);
setup.exec(`
  CREATE TABLE users (
    id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
  CREATE TABLE posts (
    id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
    body TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')));
  CREATE TABLE likes (
    user_id INTEGER NOT NULL REFERENCES users(id), post_id INTEGER NOT NULL REFERENCES posts(id),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    PRIMARY KEY (user_id, post_id));
  CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
  CREATE INDEX posts_user_id_idx       ON posts (user_id);
  CREATE INDEX likes_post_id_idx       ON likes (post_id);
  BEGIN;
  WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000)
    INSERT INTO users (id, username) SELECT i, 'user' || i FROM n;
  WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100000)
    INSERT INTO posts (id, user_id, body, created_at)
    SELECT i, 1 + i % 20000, 'post ' || hex(randomblob(1 + i % 60)),
           strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '-' || (100000 - i) || ' seconds') FROM n;
  WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 400000)
    INSERT OR IGNORE INTO likes (user_id, post_id)
    SELECT 1 + abs(random()) % 20000, 1 + abs(random()) % 100000 FROM n;
  COMMIT;
  ANALYZE;`);

Deno.env.set("SQLITE_PATH", `${dir}/pgo.db`);
Deno.env.set("JWT_SECRET", SECRET);
const { handle } = await import("../server.ts");

const b64 = (o: unknown) =>
  Buffer.from(JSON.stringify(o)).toString("base64url");
const tokens = Array.from({ length: 1000 }, (_, i) => {
  const signed = b64({ alg: "HS256", typ: "JWT" }) + "." +
    b64({ sub: String(i + 1), username: `user${i + 1}`, iat: 0, exp: 2e9 });
  return `Bearer ${signed}.${
    createHmac("sha256", SECRET).update(signed).digest("base64url")
  }`;
});
const rnd = (n: number) => 1 + Math.floor(Math.random() * n);
const call = (method: string, path: string, auth?: string, body?: string) =>
  handle(method, path, auth ?? null, new TextEncoder().encode(body ?? ""));

for (let i = 1; i <= 100_000; i++) {
  call("GET", "/feed");
  call("GET", `/posts/${99_980 + rnd(20)}`);
  if (i % 7 === 0) {
    call(
      "POST",
      `/posts/${99_900 + rnd(100)}/like`,
      tokens[rnd(1000) - 1],
    );
  }
  if (i % 50 === 0) {
    call(
      "POST",
      "/posts",
      tokens[rnd(1000) - 1],
      JSON.stringify({ body: `pgo post ${i}` }),
    );
  }
}
await Deno.remove(dir, { recursive: true });
