REPLACE INTO temp.out
SELECT 1,
  CASE WHEN NOT idok THEN '400 Bad Request' WHEN j IS NULL THEN '404 Not Found' ELSE '200 OK' END,
  CASE WHEN NOT idok THEN '{"error":"invalid post id"}' WHEN j IS NULL THEN '{"error":"post not found"}' ELSE '{"post":' || j || '}' END
FROM (SELECT idok, CASE WHEN idok THEN (SELECT j FROM temp.pj WHERE id = CAST(pid AS INTEGER)) END AS j FROM temp.q);
