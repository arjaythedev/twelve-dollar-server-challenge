## server.m: the feed API, answered by GNU Octave.
## ox.oct (ox.cc) moves bytes and runs SQLite statements. Everything else is here.
1;

function r = reply (status, body)
  r = ["HTTP/1.1 " status "\r\nContent-Type: application/json\r\nContent-Length: " ...
       sprintf("%d", numel (body)) "\r\n\r\n" body];
endfunction

function r = bad (status, msg)
  r = reply (status, ["{\"error\":\"" msg "\"}"]);
endfunction

## A positive decimal integer without its leading zeros, or "" if S is not one.
function s = posint (s)
  if (isempty (s) || ! all (s >= "0" & s <= "9"))
    s = "";
  else
    s = s(find (s != "0", 1):end);
  endif
endfunction

## base64url, as matrix arithmetic: 3 bytes are one 24-bit number, which is 4 digits in base 64.
function s = b64enc (b)
  global B64
  n = numel (b);
  b(end+1:3*ceil (n/3)) = 0;
  x = [65536 256 1] * reshape (b, 3, []);
  s = B64(1 + [floor(x / 262144); mod(floor (x / 4096), 64); mod(floor (x / 64), 64); mod(x, 64)])(:)';
  s = s(1:ceil (4*n/3));
endfunction

function s = b64dec (t)
  global B64INV
  v = B64INV(double (t) + 1);
  n = numel (v);
  if (any (v < 0) || mod (n, 4) == 1)
    error ("bad base64url");
  endif
  v(end+1:4*ceil (n/4)) = 0;
  x = [262144 4096 64 1] * reshape (v, 4, []);
  s = char ([floor(x / 65536); mod(floor (x / 256), 256); mod(x, 256)](:)');
  s = s(1:floor (3*n/4));
endfunction

function b = unhex (h)
  d = double (h) - 48;
  d -= 39 * (d > 9);
  b = d(1:2:end) * 16 + d(2:2:end);
endfunction

## {id, username} of the request's token, or the 401 message as a string.
function u = auth (head, lhead)
  global IPAD OPAD
  k = strfind (lhead, "\r\nauthorization:");
  u = "missing bearer token";
  if (isempty (k))
    return;
  endif
  v = head(k(1)+16:end);
  v = v(1:find (v == "\r", 1) - 1);
  v = v(find (v != " " & v != "\t", 1):end);
  if (! strncmp (v, "Bearer ", 7))
    return;
  endif
  t = v(8:end);
  u = "invalid or expired token";
  d = find (t == ".");
  if (numel (d) != 2)
    return;
  endif
  signed = t(1:d(2)-1);
  mac = unhex (hash ("sha256", [OPAD char(unhex (hash ("sha256", [IPAD signed])))]));
  sig = t(d(2)+1:end);
  want = b64enc (mac);
  if (numel (sig) != numel (want) || any (sig != want))  # every byte compared: no early exit to time
    return;
  endif
  try
    h = jsondecode (b64dec (t(1:d(1)-1)), "makeValidName", false);
    p = jsondecode (b64dec (t(d(1)+1:d(2)-1)), "makeValidName", false);
  catch
    return;
  end_try_catch
  if (! (isstruct (h) && isscalar (h) && isfield (h, "alg") && strcmp (h.alg, "HS256")
         && isstruct (p) && isscalar (p) && isfield (p, "exp")))
    return;
  endif
  now = time ();
  e = p.exp;
  if (! (isnumeric (e) && isscalar (e) && e > now))
    return;
  endif
  if (isfield (p, "nbf") && ! (isnumeric (p.nbf) && isscalar (p.nbf) && p.nbf <= now))
    return;
  endif
  if (isfield (p, "sub") && ischar (p.sub) && isfield (p, "username") && ischar (p.username)
      && ! isempty (sub = posint (p.sub)))
    u = {sub, p.username};
  else
    u = "invalid token payload";
  endif
endfunction

function r = create (u, body)
  r = bad ("400 Bad Request", "malformed JSON body");
  try
    j = jsondecode (body, "makeValidName", false);  # else the key "body " would become body
  catch
    return;
  end_try_catch
  t = strtrim (body);
  ## jsondecode turns [{...}] into a struct too; only an object counts.
  if (! (t(1) == "{" && isstruct (j) && isfield (j, "body") && ischar (j.body)
         && ! isempty (b = strtrim (j.body))))
    r = bad ("400 Bad Request", "body is required");
  elseif (sum (bitand (double (b), 192) != 128) > 500)  # characters, not UTF-8 bytes
    r = bad ("400 Bad Request", "body must be at most 500 characters");
  else
    row = ox ("q", ["INSERT INTO posts (user_id, body) VALUES (CAST(?1 AS INTEGER), ?2) RETURNING " ...
                    "json_object('id', id, 'body', body, 'created_at', created_at, 'author', ?3, 'like_count', 0)"],
              u{1}, b, u{2});
    r = reply ("201 Created", ["{\"post\":" row{1} "}"]);
  endif
endfunction

function r = like (u, id)
  [~, n] = ox ("q", ["INSERT INTO likes (user_id, post_id) SELECT CAST(?1 AS INTEGER), id FROM posts " ...
                     "WHERE id = CAST(?2 AS INTEGER) ON CONFLICT (user_id, post_id) DO NOTHING"], u{1}, id);
  if (n == 1)
    r = reply ("201 Created", ["{\"liked\":true,\"already_liked\":false,\"post_id\":" id "}"]);
  elseif (! isempty (ox ("q", "SELECT 1 FROM posts WHERE id = CAST(?1 AS INTEGER)", id)))
    r = reply ("200 OK", ["{\"liked\":true,\"already_liked\":true,\"post_id\":" id "}"]);
  else
    r = bad ("404 Not Found", "post not found");
  endif
endfunction

## One request: HEAD is the request line and headers (ending in \r\n), BODY the Content-Length bytes.
function r = handle (head, body)
  global POST_SQL FEED_SQL T0
  sp = find (head(1:find (head == "\r", 1)) == " ");
  method = head(1:sp(1)-1);
  path = head(sp(1)+1:sp(2)-1);
  if (any (q = (path == "?")))
    path = path(1:find (q, 1) - 1);
  endif
  if (strcmp (method, "GET"))
    if (strcmp (path, "/feed"))
      row = ox ("q", FEED_SQL);
      r = reply ("200 OK", row{1});
    elseif (strncmp (path, "/posts/", 7) && numel (path) > 7)
      id = posint (path(8:end));
      if (isempty (id))
        r = bad ("400 Bad Request", "invalid post id");
      elseif (isempty (row = ox ("q", POST_SQL, id)))
        r = bad ("404 Not Found", "post not found");
      else
        r = reply ("200 OK", ["{\"post\":" row{1} "}"]);
      endif
    elseif (strcmp (path, "/health"))
      try
        ox ("q", "SELECT 1");
        r = reply ("200 OK", sprintf ("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%d}", floor (time () - T0)));
      catch err
        r = reply ("503 Service Unavailable",
                   jsonencode (struct ("status", "degraded", "db", "unreachable", "error", err.message)));
      end_try_catch
    else
      r = bad ("404 Not Found", "not found");
    endif
  elseif (strcmp (method, "POST")
          && (strcmp (path, "/posts") || (strncmp (path, "/posts/", 7) && numel (path) > 12
                                          && strcmp (path(end-4:end), "/like"))))
    u = auth (head, lower (head));
    if (ischar (u))
      r = bad ("401 Unauthorized", u);
    elseif (numel (path) == 6)
      r = create (u, body);
    elseif (isempty (id = posint (path(8:end-5))))
      r = bad ("400 Bad Request", "invalid post id");
    else
      r = like (u, id);
    endif
  else
    r = bad ("404 Not Found", "not found");
  endif
endfunction

global POST_SQL FEED_SQL T0 B64 B64INV IPAD OPAD
T0 = time ();
B64 = ["A":"Z" "a":"z" "0":"9" "-_"];
B64INV = -ones (1, 256);
B64INV(double (B64) + 1) = 0:63;
key = getenv ("JWT_SECRET");
if (numel (key) > 64)
  key = char (unhex (hash ("sha256", key)));
endif
key(end+1:64) = char (0);
IPAD = char (bitxor (double (key), 54));
OPAD = char (bitxor (double (key), 92));

POST = ["SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, " ...
        "'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)) AS o " ...
        "FROM posts p JOIN users u ON u.id = p.user_id "];
POST_SQL = [POST "WHERE p.id = CAST(?1 AS INTEGER)"];
FEED_SQL = ["SELECT '{\"posts\":[' || ifnull(group_concat(o, ','), '') || ']}' FROM (" ...
            POST "ORDER BY p.created_at DESC, p.id DESC LIMIT 20)"];

ox ("open", getenv ("SQLITE_PATH"));
ox ("q", "PRAGMA journal_mode=WAL");
ox ("q", "PRAGMA synchronous=NORMAL");
ox ("listen", getenv ("HOST"), str2double (getenv ("PORT")));
printf ("octave %s serving on %s:%s\n", version (), getenv ("HOST"), getenv ("PORT"));
fflush (stdout);

bufs = repmat ({""}, 1, 65537);  # pending input per descriptor (index fd + 1)
while (true)
  [fds, chunks] = ox ("wait", 60000);
  for i = 1:numel (fds)
    fd = fds(i);
    if (fd < 0)
      ox ("close", -fd);
      bufs{1-fd} = "";
      continue;
    endif
    b = [bufs{fd+1} chunks{i}];
    out = "";
    hangup = false;
    while (! isempty (e = strfind (b, "\r\n\r\n")))
      e = e(1);
      head = b(1:e+1);
      lhead = lower (head);
      if (! isempty (strfind (lhead, "\r\ntransfer-encoding:")))  # chunked bodies are not parsed: refuse, hang up
        out = [out bad("411 Length Required", "length required")];
        hangup = true;
        break;
      endif
      k = strfind (lhead, "\r\ncontent-length:");
      n = 0;
      if (! isempty (k))
        n = max ([0 sscanf(head(k(1)+17:end), "%d", 1)]);
      endif
      if (numel (b) < e + 3 + n)
        break;
      endif
      try
        out = [out handle(head, b(e+4:e+3+n))];
      catch
        out = [out bad("500 Internal Server Error", "internal server error")];
      end_try_catch
      b = b(e+4+n:end);
    endwhile
    if (numel (b) > 1048576)  # no request of this API is that large
      ox ("close", fd);
      bufs{fd+1} = "";
      continue;
    endif
    bufs{fd+1} = b;
    if (! isempty (out) && ! ox ("send", fd, out))
      bufs{fd+1} = "";
    elseif (hangup)
      ox ("close", fd);
      bufs{fd+1} = "";
    endif
  endfor
endwhile
