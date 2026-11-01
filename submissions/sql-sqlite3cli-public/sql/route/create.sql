.read route/auth.sql
INSERT INTO posts (user_id, body)
SELECT uid, tb FROM temp.jv, temp.cb WHERE err IS NULL AND length(tb) BETWEEN 1 AND 500;
REPLACE INTO temp.out
SELECT 1,
  CASE WHEN err IS NOT NULL THEN '401 Unauthorized' WHEN changes() THEN '201 Created' ELSE '400 Bad Request' END,
  CASE WHEN err IS NOT NULL THEN '{"error":"' || err || '"}'
       WHEN changes() THEN (SELECT json_object('post', json_object('id', id, 'body', body, 'created_at', created_at,
                                   'author', uname, 'like_count', 0)) FROM posts WHERE id = last_insert_rowid())
       WHEN ok IS NOT 1 THEN '{"error":"malformed JSON body"}'
       WHEN bt IS NOT 'text' OR tb = '' THEN '{"error":"body is required"}'
       ELSE '{"error":"body must be at most 500 characters"}' END
FROM temp.jv, temp.cb;
