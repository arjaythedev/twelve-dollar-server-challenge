import Database from "better-sqlite3";

import { SQLITE_PATH } from "./config.js";

const db = new Database(SQLITE_PATH);
db.pragma("locking_mode = EXCLUSIVE");
db.pragma("journal_mode = WAL");
db.pragma("synchronous = NORMAL");
db.pragma("foreign_keys = OFF");
db.pragma("cache_size = -65536");
db.pragma("mmap_size = 1073741824");
db.pragma("temp_store = MEMORY");
db.pragma("journal_size_limit = 67108864");
db.pragma("wal_autocheckpoint = 10000");

const POST_JSON = `json_object(
  'id', p.id,
  'body', p.body,
  'created_at', p.created_at,
  'author', u.username,
  'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
)`;

export const feedStmt = db
  .prepare<[], Buffer>(
    `SELECT CAST('{"posts":[' || ifnull(group_concat(j, ','), '') || ']}' AS BLOB)
     FROM (
       SELECT ${POST_JSON} AS j
       FROM posts p
       JOIN users u ON u.id = p.user_id
       ORDER BY p.created_at DESC, p.id DESC
       LIMIT 20
     )`,
  )
  .pluck();

export const postStmt = db
  .prepare<[number], Buffer>(
    `SELECT CAST('{"post":' || ${POST_JSON} || '}' AS BLOB)
     FROM posts p
     JOIN users u ON u.id = p.user_id
     WHERE p.id = ?`,
  )
  .pluck();

export const insertPostStmt = db
  .prepare<[number, string, string], Buffer>(
    `INSERT INTO posts (user_id, body)
     VALUES (?, ?)
     RETURNING CAST('{"post":' || json_object(
       'id', id,
       'body', body,
       'created_at', created_at,
       'author', ?,
       'like_count', 0
     ) || '}' AS BLOB)`,
  )
  .pluck();

export const insertLikeStmt = db.prepare<[number, number, number]>(
  `INSERT INTO likes (user_id, post_id)
   SELECT ?, ?
   WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)
   ON CONFLICT (user_id, post_id) DO NOTHING`,
);

export const postExistsStmt = db
  .prepare<[number], number>("SELECT 1 FROM posts WHERE id = ?")
  .pluck();

export const healthStmt = db.prepare<[], number>("SELECT 1").pluck();
