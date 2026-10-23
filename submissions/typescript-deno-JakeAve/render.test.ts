// render.c must write exactly the bytes SQLite's json_object did: compare both on posts full of escapes.
//   deno test --allow-ffi --allow-read --allow-write render.test.ts   (after build.sh)
import {
  deepStrictEqual as assertEquals,
  throws as assertThrows,
} from "node:assert";
import { ckptStart, ckptStats, Db } from "./sqlite.ts";

Deno.test("render_posts matches json_object byte for byte", () => {
  const dir = Deno.makeTempDirSync();
  const db = new Db(`${dir}/t.db`);
  db.exec(`CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT NOT NULL);
    CREATE TABLE posts (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, body TEXT NOT NULL, created_at TEXT NOT NULL);
    CREATE TABLE likes (user_id INTEGER NOT NULL, post_id INTEGER NOT NULL, PRIMARY KEY (user_id, post_id));`);
  const bodies = [
    'say "hi"',
    "back\\slash",
    "it's",
    "tab\there\nnew\rcr\bbs\fff",
    "ctl \x01\x1f\x7f end",
    "émoji 🎉 日本",
    "x".repeat(500),
    "\x00".repeat(3), // NUL inside TEXT: SQLite escapes it as \u0000
    '"\\'.repeat(250),
  ];
  const user = db.prepare("INSERT INTO users (id, username) VALUES (?, ?)");
  user.int(1, 1).text(2, 'o"neil\\').step();
  user.done();
  user.int(1, 2).text(2, "plain").step();
  user.done();
  const post = db.prepare(
    "INSERT INTO posts (user_id, body, created_at) VALUES (?, ?, ?)",
  );
  bodies.forEach((b, i) => {
    post.int(1, 1 + i % 2).text(2, b).text(
      3,
      `2026-10-14T00:00:${String(i).padStart(2, "0")}.000Z`,
    ).step();
    post.done();
  });
  db.exec(
    "INSERT INTO likes SELECT u.id, p.id FROM users u, posts p WHERE p.id % 3 != 0",
  );

  const JSON_ =
    `json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
    'like_count',(SELECT count(*) FROM likes l WHERE l.post_id = p.id))`;
  const ROW = `SELECT p.id, p.body, p.created_at, u.username,
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id`;
  const sqlBlob = (sql: string, id?: number) => {
    const s = db.prepare(sql);
    if (id !== undefined) s.int(1, id);
    const b = s.step() ? s.blob(0) : undefined;
    s.done();
    return b;
  };
  const out = new Uint8Array(128 * 1024);
  const rendered = (sql: string, single: boolean, id?: number) => {
    const s = db.prepare(sql);
    if (id !== undefined) s.int(1, id);
    const n = s.render(out, single);
    return n === 0 ? undefined : out.slice(0, n);
  };

  for (let id = 1; id <= bodies.length + 1; id++) { // the last id does not exist
    assertEquals(
      rendered(`${ROW} WHERE p.id = ?`, true, id),
      sqlBlob(
        `SELECT CAST('{"post":' || ${JSON_} || '}' AS BLOB) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?`,
        id,
      ),
      `post ${id}`,
    );
  }
  const FEED_ORDER = "ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
  assertEquals(
    rendered(`${ROW} ${FEED_ORDER}`, false),
    sqlBlob(
      `SELECT CAST('{"posts":[' || ifnull(group_concat(j, ','), '') || ']}' AS BLOB)
      FROM (SELECT ${JSON_} AS j FROM posts p JOIN users u ON u.id = p.user_id ${FEED_ORDER})`,
    ),
  );
  assertEquals(
    new TextDecoder().decode(rendered(`${ROW} WHERE 0 ${FEED_ORDER}`, false)),
    '{"posts":[]}',
  );
  const small = db.prepare(`${ROW} ${FEED_ORDER}`);
  assertThrows(() => small.render(new Uint8Array(100), false));
  Deno.removeSync(dir, { recursive: true });
});

Deno.test("checkpointer backfills the WAL", async () => {
  const dir = Deno.makeTempDirSync();
  const path = `${dir}/t.db`;
  // No EXCLUSIVE locking: the checkpointer's connection needs the shared-memory WAL index.
  const db = new Db(path);
  db.exec(
    `PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL; PRAGMA wal_autocheckpoint = 0;
    CREATE TABLE t (x TEXT)`,
  );
  const ins = db.prepare("INSERT INTO t VALUES (?)");
  for (let i = 0; i < 200; i++) {
    ins.text(1, "x".repeat(400)).step();
    ins.done();
  }
  ckptStart(path, 50, 2);
  assertThrows(() => ckptStart(path, 50, 2)); // one per process
  await new Promise((r) => setTimeout(r, 300));
  const s = ckptStats();
  assertEquals(
    s.runs > 0 && s.log > 0 && s.ckpt === s.log,
    true,
    JSON.stringify(s),
  );
  Deno.removeSync(dir, { recursive: true });
});
