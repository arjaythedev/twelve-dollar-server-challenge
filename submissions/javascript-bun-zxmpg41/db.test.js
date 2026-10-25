import { afterAll, expect, test } from "bun:test";
import { Database } from "bun:sqlite";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const dir = mkdtempSync(join(tmpdir(), "bun-group-commit-"));
const path = join(dir, "feed.db");
const reader = new Database(path);
reader.exec(readFileSync(new URL("../../schema.sql", import.meta.url), "utf8"));
reader.exec("INSERT INTO users (id, username) VALUES (1, 'test_user')");
const previousPath = process.env.SQLITE_PATH;
process.env.SQLITE_PATH = path;
const db = await import("./db.js");
if (previousPath === undefined) delete process.env.SQLITE_PATH;
else process.env.SQLITE_PATH = previousPath;

const countPosts = () => reader.query("SELECT count(*) AS n FROM posts").get().n;

afterAll(() => {
  reader.close();
  rmSync(dir, { recursive: true, force: true });
});

test("a sparse write flushes and is committed before its promise resolves", async () => {
  const pending = db.createPost(1, "sparse");
  expect(countPosts()).toBe(0);
  const row = await pending;
  expect(reader.query("SELECT body FROM posts WHERE id = ?").get(row.id).body).toBe("sparse");
});

test("a burst is split into bounded transactions and drains in order", async () => {
  const before = countPosts();
  const pending = Array.from({ length: 150 }, (_, i) => db.createPost(1, `burst ${i}`));
  const first = await pending[0];
  // Promise continuations run before the next setImmediate batch. A separate
  // connection sees only committed rows, not the writer's uncommitted work.
  const committed = countPosts() - before;
  expect(committed).toBeGreaterThan(0);
  expect(committed).toBeLessThanOrEqual(64);
  const rows = await Promise.all(pending);
  expect(countPosts() - before).toBe(150);
  expect(rows.map((row) => row.id)).toEqual(Array.from({ length: 150 }, (_, i) => first.id + i));
});

test("a statement failure does not lose other writes or stop subsequent flushes", async () => {
  const before = countPosts();
  const results = await Promise.allSettled([
    db.createPost(1, "before invalid"),
    db.createPost(1, ""),
    db.createPost(1, "after invalid"),
  ]);
  expect(results.map((result) => result.status)).toEqual(["fulfilled", "rejected", "fulfilled"]);
  expect(countPosts() - before).toBe(2);
  await db.createPost(1, "next flush");
  expect(countPosts() - before).toBe(3);
});

test("the execution budget yields after a slow statement without dropping the backlog", async () => {
  reader.exec(`CREATE TRIGGER slow_write BEFORE INSERT ON posts
    WHEN NEW.body = 'slow budget'
    BEGIN
      SELECT sum(n) FROM (
        WITH RECURSIVE work(n) AS (VALUES(1) UNION ALL SELECT n + 1 FROM work WHERE n < 1000000)
        SELECT n FROM work
      );
    END`);
  try {
    const before = countPosts();
    const pending = [db.createPost(1, "slow budget")];
    for (let i = 0; i < 7; i++) pending.push(db.createPost(1, `after slow ${i}`));
    await pending[0];
    expect(countPosts() - before).toBe(1);
    await Promise.all(pending);
    expect(countPosts() - before).toBe(8);
  } finally {
    reader.exec("DROP TRIGGER slow_write");
  }
});

test("duplicate and missing-post likes stay correct across batches", async () => {
  const row = await db.createPost(1, "like target");
  const results = await Promise.all(Array.from({ length: 130 }, () => db.like(1, row.id)));
  expect(results).toEqual([false, ...Array(129).fill(true)]);
  expect(reader.query("SELECT count(*) AS n FROM likes WHERE post_id = ?").get(row.id).n).toBe(1);
  expect(await db.like(1, 999999999)).toBeNull();
});
