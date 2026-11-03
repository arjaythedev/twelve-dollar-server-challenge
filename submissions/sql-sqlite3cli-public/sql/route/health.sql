REPLACE INTO temp.out SELECT 1, '200 OK', '{"status":"ok","db":"ok","uptime_s":' || (unixepoch() - boot) || '}'
FROM temp.cfg, (SELECT 1);
