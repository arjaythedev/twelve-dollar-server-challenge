-- app.sql: the web application. Every sqlite3 reads this once at startup, after server.sh's boot.sql
-- has set temp_store=MEMORY (changing it later drops all temp tables) and created temp.cfg.
-- Per request, bash stores the raw head and body in temp.req; a trigger parses them into temp.q, q.route
-- names the file in route/ that runs, and temp.http is the response written to the socket.
PRAGMA busy_timeout = 5000;
PRAGMA synchronous = NORMAL;
PRAGMA cache_size = -2000;
PRAGMA mmap_size = 268435456;

CREATE TEMP TABLE req(id INTEGER PRIMARY KEY, head TEXT, body TEXT);
CREATE TEMP TABLE out(id INTEGER PRIMARY KEY, status TEXT, body TEXT);
CREATE TEMP TABLE jv(id INTEGER PRIMARY KEY, err TEXT, uid INT, uname TEXT);

-- The HTTP parser is a trigger: inserting a request fills temp.q one stage per UPDATE. (As nested
-- subqueries it was ~1.5 ms a request: the query flattener copies each column's expression into
-- every use, and through seven levels the copies multiply.)
CREATE TEMP TABLE q(id INTEGER PRIMARY KEY, hd, l1, ap, r1, tg,
  m, path, auth, pid, idok, route,  -- request line, Authorization, post id, route
  t, d1, r, d2, bearer, h, p, s, shaped);  -- the bearer token as header.payload.signature
CREATE TEMP TRIGGER parse AFTER INSERT ON req BEGIN
  DELETE FROM out;  -- a failed route must not resend the last response
  REPLACE INTO q(id, hd) VALUES (1, replace(new.head, char(13), ''));
  UPDATE q SET l1 = substr(hd, 1, instr(hd || char(10), char(10)) - 1), ap = instr(lower(hd), char(10) || 'authorization:');
  UPDATE q SET m = substr(l1, 1, instr(l1, ' ') - 1), r1 = substr(l1, instr(l1, ' ') + 1),
    auth = CASE WHEN ap THEN trim(substr(hd, ap + 15, instr(substr(hd, ap + 15) || char(10), char(10)) - 1)) END;
  UPDATE q SET tg = substr(r1, 1, instr(r1 || ' ', ' ') - 1), t = substr(auth, 8), bearer = auth GLOB 'Bearer *';
  UPDATE q SET path = CASE WHEN instr(tg, '?') THEN substr(tg, 1, instr(tg, '?') - 1) ELSE tg END,
    d1 = instr(t, '.'), r = substr(t, instr(t, '.') + 1);
  UPDATE q SET pid = CASE WHEN path GLOB '/posts/*/like' THEN substr(path, 8, length(path) - 12) ELSE substr(path, 8) END,
    h = substr(t, 1, d1 - 1), d2 = instr(r, '.');
  UPDATE q SET idok = pid GLOB '[1-9]*' AND pid NOT GLOB '*[^0-9]*' AND length(pid) <= 18,
    route = CASE
      WHEN m = 'GET' AND path = '/feed' THEN 'feed'
      WHEN m = 'GET' AND path = '/health' THEN 'health'
      WHEN m = 'GET' AND path GLOB '/posts/*' AND pid NOT GLOB '*/*' THEN 'post'
      WHEN m = 'POST' AND path = '/posts' THEN 'create'
      WHEN m = 'POST' AND path GLOB '/posts/*/like' AND pid NOT GLOB '*/*' THEN 'like'
      ELSE 'notfound' END,
    p = substr(r, 1, d2 - 1), s = substr(r, d2 + 1);
  UPDATE q SET shaped = d1 > 1 AND d2 > 1 AND s <> '' AND (h || p || s) NOT GLOB '*[^A-Za-z0-9_-]*';
END;
CREATE TEMP VIEW jr AS SELECT h || '.' || p AS m FROM temp.q;
.read hmac.sql

-- the POST /posts body
CREATE TEMP VIEW cb AS
SELECT ok, bt, CASE WHEN bt = 'text' THEN trim(json_extract(body, '$.body'), char(32, 9, 10, 13)) END AS tb
FROM (SELECT body, json_valid(body) AS ok, CASE WHEN json_valid(body) THEN json_type(body, '$.body') END AS bt FROM temp.req);

-- post objects, by id and newest first
CREATE TEMP VIEW pj AS
SELECT p.id AS id, p.created_at AS created_at,
  json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
              'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)) AS j
FROM posts p JOIN users u ON u.id = p.user_id;
CREATE TEMP VIEW feed AS
SELECT '{"posts":[' || group_concat(j, ',') || ']}' AS b
FROM (SELECT j FROM temp.pj ORDER BY created_at DESC, id DESC LIMIT 20);

-- the response; sqlite3 ends the row with "\n", which counts as the body's trailing newline
CREATE TEMP VIEW http AS
SELECT 'HTTP/1.1 ' || status || char(13, 10) || 'Content-Type: application/json' || char(13, 10) ||
  'Content-Length: ' || (octet_length(body) + 1) || char(13, 10) || char(13, 10) || body AS r
FROM (SELECT coalesce(o.status, CASE WHEN q.route = 'health' THEN '503 Service Unavailable' ELSE '500 Internal Server Error' END) AS status,
        coalesce(o.body, CASE WHEN q.route = 'health' THEN '{"status":"degraded","db":"unreachable","error":"query failed"}'
                              ELSE '{"error":"internal server error"}' END) AS body
  FROM temp.q q LEFT JOIN temp.out o);
