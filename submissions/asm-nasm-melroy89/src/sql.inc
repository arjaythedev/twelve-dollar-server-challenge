; SPDX-License-Identifier: MIT
; SQL constants, byte encoding avoids assembler quoting ambiguity.
settings: db `PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA cache_size=500; PRAGMA mmap_size=1073741824; PRAGMA wal_autocheckpoint=0;`,0
q_wal: db `PRAGMA journal_mode`,0
q_sync: db `PRAGMA synchronous`,0
q_fk: db `PRAGMA foreign_keys`,0
q_ping: db `SELECT 1`,0
q_feed: db `SELECT '{"posts":['||coalesce(group_concat(entry),'')||']}' FROM (SELECT json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id)) AS entry FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20)`,0
q_post: db `SELECT '{"post":'||json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))||'}' FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1`,0
q_insert: db `INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING id,created_at`,0
q_like: db `INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING`,0
q_exists: db `SELECT 1 FROM posts WHERE id=?1`,0
q_begin: db `BEGIN`,0
q_commit: db `COMMIT`,0
q_rollback: db `ROLLBACK`,0
q_checkpoint: db `PRAGMA wal_checkpoint(PASSIVE)`,0
q_savepoint: db 'SAVEPOINT request',0
q_release: db 'RELEASE request',0
q_rollbackto: db 'ROLLBACK TO request',0
queries: dq q_ping,q_feed,q_post,q_insert,q_like,q_exists,q_begin,q_commit,q_rollback,q_checkpoint,q_savepoint,q_release,q_rollbackto

; Query experiments, all preparation remains at startup.
q_feed_plain: db `SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20`,0
q_post_plain: db `SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1`,0
q_feed_selected_json: db `WITH selected AS MATERIALIZED(SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20), counts AS (SELECT l.post_id,count(*) AS n FROM selected s JOIN likes l ON l.post_id=s.id GROUP BY l.post_id) SELECT '{"posts":['||coalesce(group_concat(entry),'')||']}' FROM (SELECT json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',p.username,'like_count',coalesce(c.n,0)) AS entry FROM selected p LEFT JOIN counts c ON c.post_id=p.id ORDER BY p.created_at DESC,p.id DESC)`,0
q_feed_selected_plain: db `WITH selected AS MATERIALIZED(SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20), counts AS (SELECT l.post_id,count(*) AS n FROM selected s JOIN likes l ON l.post_id=s.id GROUP BY l.post_id) SELECT p.id,p.body,p.created_at,p.username,coalesce(c.n,0) FROM selected p LEFT JOIN counts c ON c.post_id=p.id ORDER BY p.created_at DESC,p.id DESC`,0
q_feed_range_json: db `WITH selected AS MATERIALIZED(SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20), bounds AS (SELECT min(id) AS lo,max(id) AS hi FROM selected),counts AS (SELECT l.post_id,count(*) AS n FROM likes l WHERE l.post_id BETWEEN (SELECT lo FROM bounds) AND (SELECT hi FROM bounds) AND (SELECT hi-lo FROM bounds)<256 GROUP BY l.post_id) SELECT '{"posts":['||coalesce(group_concat(entry),'')||']}' FROM (SELECT json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',p.username,'like_count',CASE WHEN (SELECT hi-lo FROM bounds)<256 THEN coalesce(c.n,0) ELSE (SELECT count(*) FROM likes l WHERE l.post_id=p.id) END) AS entry FROM selected p LEFT JOIN counts c ON c.post_id=p.id ORDER BY p.created_at DESC,p.id DESC)`,0
q_feed_range_plain: db `WITH selected AS MATERIALIZED(SELECT p.id,p.body,p.created_at,u.username FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20), bounds AS (SELECT min(id) AS lo,max(id) AS hi FROM selected),counts AS (SELECT l.post_id,count(*) AS n FROM likes l WHERE l.post_id BETWEEN (SELECT lo FROM bounds) AND (SELECT hi FROM bounds) AND (SELECT hi-lo FROM bounds)<256 GROUP BY l.post_id) SELECT p.id,p.body,p.created_at,p.username,CASE WHEN (SELECT hi-lo FROM bounds)<256 THEN coalesce(c.n,0) ELSE (SELECT count(*) FROM likes l WHERE l.post_id=p.id) END FROM selected p LEFT JOIN counts c ON c.post_id=p.id ORDER BY p.created_at DESC,p.id DESC`,0
