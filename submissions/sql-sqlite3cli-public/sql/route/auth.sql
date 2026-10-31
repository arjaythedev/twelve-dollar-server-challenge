-- the bearer token, checked into temp.jv: err (NULL when valid), uid, uname
REPLACE INTO temp.jv
SELECT 1,
  CASE
    WHEN bearer IS NOT 1 THEN 'missing bearer token'
    WHEN shaped IS NOT 1 THEN 'invalid or expired token'
    WHEN (SELECT sig FROM temp.jsig) IS NOT s THEN 'invalid or expired token'
    WHEN json_valid(hj) IS NOT 1 THEN 'invalid or expired token'
    WHEN json_type(hj) IS NOT 'object' OR json_extract(hj, '$.alg') IS NOT 'HS256' THEN 'invalid or expired token'
    WHEN json_valid(pj) IS NOT 1 THEN 'invalid or expired token'
    WHEN json_type(pj) IS NOT 'object' THEN 'invalid or expired token'
    WHEN json_type(pj, '$.exp') IS NOT 'integer' AND json_type(pj, '$.exp') IS NOT 'real' THEN 'invalid or expired token'
    WHEN json_extract(pj, '$.exp') <= unixepoch() THEN 'invalid or expired token'
    WHEN json_type(pj, '$.sub') IS NOT 'text' THEN 'invalid token payload'
    WHEN json_extract(pj, '$.sub') NOT GLOB '[1-9]*' OR json_extract(pj, '$.sub') GLOB '*[^0-9]*'
      OR length(json_extract(pj, '$.sub')) > 18 THEN 'invalid token payload'
    WHEN json_type(pj, '$.username') IS NOT 'text' THEN 'invalid token payload'
  END,
  CASE WHEN json_valid(pj) THEN json_extract(pj, '$.sub') END,
  CASE WHEN json_valid(pj) THEN json_extract(pj, '$.username') END
FROM (SELECT *, CAST(base64(coalesce(replace(replace(h, '-', '+'), '_', '/'), '')) AS TEXT) AS hj,
                CAST(base64(coalesce(replace(replace(p, '-', '+'), '_', '/'), '')) AS TEXT) AS pj FROM temp.q);
