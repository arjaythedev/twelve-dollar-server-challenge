class Post {
    id: int,
    body: string,
    created_at: string,
    author: string,
    like_count: int,
}

class QueryResult {
    rows: baml.json.json[][],
    changes: int,
}

class User {
    id: int,
    username: string,
}

class Token {
    sub: baml.json.json?,
    username: baml.json.json?,
    exp: baml.json.json?,
    nbf: baml.json.json?,
}

class TokenHeader {
    alg: string,
}

class Body {
    body: baml.json.json?,
}

function response(status: int, body: string) -> baml.http.Response {
    baml.http.Response.new(status, { "content-type": "application/json" }, body.to_utf8())
}

function error(status: int, message: string) -> baml.http.Response throws never {
    response(status, "{\"error\":" + baml.json.stringify(message) + "}")
}

function query(path: string, sql: string, params: baml.json.json[]) -> QueryResult {
    baml.json.from_string<QueryResult>(baml.sqlite.query(path, sql, baml.json.stringify(params)))
}

function select_post() -> string {
    "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id"
}

function post(row: baml.json.json[]) -> Post {
    Post {
        id: baml.json.to<int>(row[0]),
        body: baml.json.to<string>(row[1]),
        created_at: baml.json.to<string>(row[2]),
        author: baml.json.to<string>(row[3]),
        like_count: baml.json.to<int>(row[4]),
    }
}

function positive_id(raw: string) -> int? {
    if (!baml.regex.new("^[0-9]+$").has_match(raw)) {
        return null;
    }
    let n = int.parse(raw) catch (e) {
        _ => 0
    };
    if (n > 0) {
        n
    } else {
        null
    }
}

function decode64(raw: string) -> string {
    if (!baml.regex.new("^[A-Za-z0-9_-]*$").has_match(raw)) {
        baml.sys.panic("base64url");
    }
    let s = raw.replace_all("-", "+").replace_all("_", "/");
    let padding = (4 - s.length() % 4) % 4;
    let padded = s;
    for (let i = 0; i < padding; i += 1) {
        padded = padded + "=";
    }
    baml.Uint8Array.from_base64(padded).to_string()
}

function hmac(secret: string, message: string) -> string {
    let key = secret.to_utf8();
    if (key.length() > 64) {
        let hash = baml.crypto.Sha256.new();
        hash.update(key);
        key = hash.finish();
    }
    let inner_pad = b"";
    let outer_pad = b"";
    for (let i = 0; i < 64; i += 1) {
        let byte = key.at(i) ?? 0;
        inner_pad.push(byte ^ 54);
        outer_pad.push(byte ^ 92);
    }
    let inner = baml.crypto.Sha256.new();
    inner.update(inner_pad);
    inner.update(message.to_utf8());
    let outer = baml.crypto.Sha256.new();
    outer.update(outer_pad);
    outer.update(inner.finish());
    outer.finish().to_base64().replace_all("+", "-").replace_all("/", "_").replace_all("=", "")
}

function authenticate(header: string, secret: string) -> User | baml.http.Response {
    let token = header.slice(7, header.length()).split(".");
    if (token.length() != 3) {
        return error(401, "invalid or expired token");
    }
    let head = baml.json.from_string<TokenHeader>(decode64(token[0]));
    if (head.alg != "HS256") {
        return error(401, "invalid or expired token");
    }
    let expected = hmac(secret, token[0] + "." + token[1]);
    // Compare every byte of equal-length signatures, without early exit.
    if (expected.length() != token[2].length()) {
        return error(401, "invalid or expired token");
    }
    let difference = 0;
    let a = expected.to_utf8();
    let b = token[2].to_utf8();
    for (let i = 0; i < a.length(); i += 1) {
        difference = difference | ((a.at(i) ?? 0) ^ (b.at(i) ?? 0));
    }
    if (difference != 0) {
        return error(401, "invalid or expired token");
    }
    let payload = baml.json.from_string<Token>(decode64(token[1]));
    let now = baml.time.Instant.now().to_timestamp_seconds().to_int();
    let valid_exp = match (payload.exp) {
        let n: int => now < n,
        let n: float => float.parse(string.from(now)) < n,
        _ => false,
    };
    if (!valid_exp) {
        return error(401, "invalid or expired token");
    }
    if (payload.nbf != null) {
        let valid_nbf = match (payload.nbf) {
            let n: int => now >= n,
            let n: float => float.parse(string.from(now)) >= n,
            _ => false,
        };
        if (!valid_nbf) {
            return error(401, "invalid or expired token");
        }
    }
    let sub = match (payload.sub) {
        let s: string => positive_id(s),
        _ => null,
    };
    let username = match (payload.username) {
        let s: string => s,
        _ => null,
    };
    if (sub == null || username == null) {
        return error(401, "invalid token payload");
    }
    User { id: sub, username: username }
}

function handle(
    req: baml.http.ServerRequest,
    path: string,
    secret: string,
    started: int,
) -> baml.http.Response {
    let url = req.url.split("?")[0];
    if (req.method == "GET" && url == "/health") {
        let healthy = query(path, "SELECT 1", []) catch (e) {
            _ => null
        };
        if (healthy == null) {
            return response(
                503,
                "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"SQLite query failed\"}",
            );
        }
        let uptime = baml.time.Instant.now().to_timestamp_seconds().to_int() - started;
        return response(200, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + string.from(uptime) + "}");
    }
    if (req.method == "GET" && url == "/feed") {
        let rows = query(path, select_post() + " ORDER BY p.created_at DESC,p.id DESC LIMIT 20", []).rows;
        let posts: Post[] = [];
        for (let row in rows) {
            posts.push(post(row));
        }
        return response(200, "{\"posts\":" + baml.json.to_string(posts) + "}");
    }
    let segments = url.split("/");
    let get_post = req.method == "GET" && segments.length() == 3 && segments[1] == "posts";
    let create = req.method == "POST" && url == "/posts";
    let like = req.method == "POST"
        && segments.length() == 4
        && segments[1] == "posts"
        && segments[3] == "like";
    if (!get_post && !create && !like) {
        return error(404, "not found");
    }
    let user = User { id: 0, username: "" };
    if (create || like) {
        let header = req.headers.get("authorization") ?? "";
        if (!header.starts_with("Bearer ")) {
            return error(401, "missing bearer token");
        }
        let auth = authenticate(header, secret) catch (e) {
            _ => error(401, "invalid or expired token")
        };
        match (auth) {
            let u: User => {
                user = u;
            },
            let r: baml.http.Response => {
                return r;
            },
        }
    }
    if (create) {
        let parsed = baml.json.parse(req.body) catch (e) {
            _ => null
        };
        if (parsed == null && req.body.trim() != "null") {
            return error(400, "malformed JSON body");
        }
        let data = baml.json.to<Body>(parsed) catch (e) {
            _ => Body { body: null }
        };
        let text = match (data.body) {
            let s: string => s.trim(),
            _ => "",
        };
        if (text.length() == 0) {
            return error(400, "body is required");
        }
        if (text.length() > 500) {
            return error(400, "body must be at most 500 characters");
        }
        let row = query(
            path,
            "INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at",
            [user.id, text],
        )
            .rows[0];
        let p = Post {
            id: baml.json.to<int>(row[0]),
            body: text,
            created_at: baml.json.to<string>(row[1]),
            author: user.username,
            like_count: 0,
        };
        return response(201, "{\"post\":" + baml.json.to_string(p) + "}");
    }
    let id = positive_id(segments[2]);
    if (id == null) {
        return error(400, "invalid post id");
    }
    if (get_post) {
        let rows = query(path, select_post() + " WHERE p.id=?", [id]).rows;
        if (rows.length() == 0) {
            return error(404, "post not found");
        }
        return response(200, "{\"post\":" + baml.json.to_string(post(rows[0])) + "}");
    }
    let inserted = query(
        path,
        "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING",
        [user.id, id],
    );
    if (
        inserted.changes == 0
            && query(path, "SELECT 1 FROM posts WHERE id=?", [id]).rows.length() == 0
    ) {
        return error(404, "post not found");
    }
    let already = inserted.changes == 0;
    response(
        if (already) {
            200
        } else {
            201
        },
        "{\"liked\":true,\"already_liked\":"
            + string.from(already)
            + ",\"post_id\":"
            + string.from(id)
            + "}",
    )
}

function main() -> void {
    let path = baml.env.get_or_panic("SQLITE_PATH");
    let secret = baml.env.get_or_panic("JWT_SECRET");
    let host = baml.env.get("HOST") ?? "127.0.0.1";
    let port = baml.env.get("PORT") ?? "3000";
    let started = baml.time.Instant.now().to_timestamp_seconds().to_int();
    query(path, "SELECT 1", []);
    let server = baml.http.Server.bind(host + ":" + port);
    server.serve(
        (req: baml.http.ServerRequest) -> baml.http.Response {
            handle(req, path, secret, started) catch (e) {
                _ => error(500, "internal server error")
            }
        },
        max_body_size = 16384,
        header_read_timeout = baml.time.Duration.from_seconds(75n),
    );
}
