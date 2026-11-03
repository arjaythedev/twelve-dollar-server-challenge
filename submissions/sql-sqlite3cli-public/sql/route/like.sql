.read route/auth.sql
INSERT INTO likes (user_id, post_id)
SELECT uid, CAST(pid AS INTEGER) FROM temp.jv, temp.q
WHERE err IS NULL AND idok AND EXISTS (SELECT 1 FROM posts WHERE id = CAST(pid AS INTEGER))
ON CONFLICT DO NOTHING;
REPLACE INTO temp.out
SELECT 1,
  CASE WHEN err IS NOT NULL THEN '401 Unauthorized' WHEN NOT idok THEN '400 Bad Request'
       WHEN changes() THEN '201 Created' WHEN found THEN '200 OK' ELSE '404 Not Found' END,
  CASE WHEN err IS NOT NULL THEN '{"error":"' || err || '"}' WHEN NOT idok THEN '{"error":"invalid post id"}'
       WHEN changes() THEN '{"liked":true,"already_liked":false,"post_id":' || pid || '}'
       WHEN found THEN '{"liked":true,"already_liked":true,"post_id":' || pid || '}'
       ELSE '{"error":"post not found"}' END
FROM (SELECT err, idok, pid, idok AND EXISTS (SELECT 1 FROM posts WHERE id = CAST(pid AS INTEGER)) AS found
      FROM temp.jv, temp.q);
