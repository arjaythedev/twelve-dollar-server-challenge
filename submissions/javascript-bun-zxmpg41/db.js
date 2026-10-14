// SQLite access: one connection, prepared statements, JSON built inside SQLite, group commit for writes.
import { Database } from "bun:sqlite";

const db = new Database(process.env.SQLITE_PATH, { strict: true });
db.exec("PRAGMA journal_mode = WAL");
db.exec("PRAGMA synchronous = NORMAL");
db.exec("PRAGMA busy_timeout = 5000");
db.exec("PRAGMA cache_size = -65536"); // 64 MB page cache
db.exec("PRAGMA mmap_size = 536870912"); // 512 MB mmap
db.exec("PRAGMA temp_store = MEMORY");

const POST_JSON = `json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
  'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))`;

const healthStmt = db.query("SELECT 1 AS ok");
const feedStmt = db.query(`
  SELECT '{"posts":' || json_group_array(json(j)) || '}' AS body FROM (
    SELECT ${POST_JSON} AS j
      FROM posts p JOIN users u ON u.id = p.user_id
     ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`);
const postStmt = db.query(`
  SELECT '{"post":' || ${POST_JSON} || '}' AS body
    FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $id`);
const insertPost = db.query("INSERT INTO posts (user_id, body) VALUES ($uid, $body) RETURNING id, created_at");
const postExists = db.query("SELECT 1 AS x FROM posts WHERE id = $id");
const insertLike = db.query("INSERT INTO likes (user_id, post_id) VALUES ($uid, $pid) ON CONFLICT (user_id, post_id) DO NOTHING");
const begin = db.query("BEGIN IMMEDIATE");
const commit = db.query("COMMIT");
const rollback = db.query("ROLLBACK");

export function health() {
  healthStmt.get();
}
export function feed() {
  return feedStmt.get().body;
}
export function post(id) {
  return postStmt.get({ id })?.body ?? null;
}

// ---- group commit: queue writes, flush them in one transaction, resolve after COMMIT ----
let queue = [];
let scheduled = false;

function enqueue(run) {
  return new Promise((resolve, reject) => {
    queue.push({ run, resolve, reject });
    if (!scheduled) {
      scheduled = true;
      setImmediate(flush);
    }
  });
}

function flush() {
  const batch = queue;
  queue = [];
  scheduled = false;
  const results = new Array(batch.length);
  try {
    begin.run();
    for (let i = 0; i < batch.length; i++) {
      // A failing statement only rolls back itself, not the surrounding transaction.
      try { results[i] = { ok: true, v: batch[i].run() }; }
      catch (e) { results[i] = { ok: false, e }; }
    }
    commit.run();
  } catch (e) {
    try { rollback.run(); } catch {}
    for (const item of batch) item.reject(e);
    return;
  }
  for (let i = 0; i < batch.length; i++) {
    const r = results[i];
    r.ok ? batch[i].resolve(r.v) : batch[i].reject(r.e);
  }
}

export function createPost(uid, body) {
  return enqueue(() => insertPost.get({ uid, body }));
}

// Resolves to null (post missing), true (already liked) or false (new like).
export function like(uid, pid) {
  return enqueue(() => {
    if (!postExists.get({ id: pid })) return null;
    return insertLike.run({ uid, pid }).changes === 0;
  });
}
