-- The API. bin/server runs this statement once per batch of requests.
--
-- http_requests (a virtual table, see app.h) has one row per request received since the last run.
-- The statement's writes commit when it finishes, and bin/server sends the replies only after that.
--
-- Functions defined in C: jwt(auth) verifies a Bearer token, http_reply(slot, '<status> <json>') queues
-- a reply, write_like() and write_post() run the INSERTs, uptime() returns seconds since start.
WITH req AS MATERIALIZED (
  SELECT slot, route, post_id, body,
         CASE WHEN route IN ('create', 'like') THEN jwt(auth) END AS who
    FROM http_requests
)
SELECT http_reply(slot, CASE

  WHEN route = 'health' THEN
    '200 {"status":"ok","db":"ok","uptime_s":' || ((SELECT 1) * uptime()) || '}'

  WHEN route = 'feed' THEN
    '200 {"posts":' || (
      SELECT json_group_array(json_object('id', id, 'body', body, 'created_at', created_at, 'author', author,
                                          'like_count', like_count)) FROM (
        SELECT p.id, p.body, p.created_at, u.username AS author,
               (SELECT count(*) FROM likes AS l WHERE l.post_id = p.id) AS like_count
          FROM posts AS p JOIN users AS u ON u.id = p.user_id
         -- Rule 5: every request reads its own data. This always-true condition refers to the request
         -- row, so SQLite reads the feed once per request. Without it the subquery is uncorrelated and
         -- SQLite reads it once per statement run, which every feed request in the batch would share:
         --   (no WHERE clause here)
         WHERE req.slot = req.slot
         ORDER BY p.created_at DESC, p.id DESC
         LIMIT 20)) || '}'

  WHEN route = 'notfound' THEN '404 {"error":"not found"}'

  WHEN route = 'post' AND req.post_id IS NULL THEN '400 {"error":"invalid post id"}'
  WHEN route = 'post' THEN coalesce(
    (SELECT '200 ' || json_object('post', json_object(
              'id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
              'like_count', (SELECT count(*) FROM likes AS l WHERE l.post_id = p.id)))
       FROM posts AS p JOIN users AS u ON u.id = p.user_id
      WHERE p.id = req.post_id),
    '404 {"error":"post not found"}')

  -- everything below needs a valid token, checked before the id
  WHEN who = 'missing' THEN '401 {"error":"missing bearer token"}'
  WHEN who = 'invalid' THEN '401 {"error":"invalid or expired token"}'
  WHEN who = 'payload' THEN '401 {"error":"invalid token payload"}'

  WHEN route = 'like' AND req.post_id IS NULL THEN '400 {"error":"invalid post id"}'
  WHEN route = 'like' AND NOT EXISTS (SELECT 1 FROM posts AS p WHERE p.id = req.post_id) THEN
    '404 {"error":"post not found"}'
  WHEN route = 'like' THEN
    CASE write_like(who ->> '$.id', req.post_id)
      WHEN 1 THEN '201 {"liked":true,"already_liked":false,"post_id":' || req.post_id || '}'
      ELSE        '200 {"liked":true,"already_liked":true,"post_id":'  || req.post_id || '}'
    END

  -- route = 'create'
  WHEN NOT json_valid(body) THEN '400 {"error":"malformed JSON body"}'
  WHEN json_type(body, '$.body') IS NOT 'text'
    OR trim(body ->> '$.body', ' ' || char(9, 10, 11, 12, 13)) = '' THEN '400 {"error":"body is required"}'
  WHEN length(trim(body ->> '$.body', ' ' || char(9, 10, 11, 12, 13))) > 500 THEN
    '400 {"error":"body must be at most 500 characters"}'
  ELSE
    '201 ' || write_post(who ->> '$.id', trim(body ->> '$.body', ' ' || char(9, 10, 11, 12, 13)), who ->> '$.name')

END) FROM req;
