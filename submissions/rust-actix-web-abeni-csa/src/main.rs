use actix_web::{App, HttpRequest, HttpResponse, HttpServer, web};
use base64::Engine;
use hmac::{Hmac, Mac};
use once_cell::sync::Lazy;
use r2d2::Pool;
use r2d2_sqlite::SqliteConnectionManager;
use rusqlite::{Connection, OptionalExtension};
use serde::Serialize;
use sha2::Sha256;
use sonic_rs::JsonValueTrait;
use std::env;
use std::sync::Mutex;
use std::thread::available_parallelism;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static JWT_SECRET: Lazy<Vec<u8>> = Lazy::new(|| {
    env::var("JWT_SECRET")
        .unwrap_or_else(|_| "twelve-dollar-challenge".into())
        .into_bytes()
});
static START: Lazy<SystemTime> = Lazy::new(SystemTime::now);
/// Read pool: many concurrent readers (WAL allows this).
static READ_POOL: Lazy<Pool<SqliteConnectionManager>> = Lazy::new(|| {
    let path = env::var("SQLITE_PATH").expect("SQLITE_PATH must be set");
    let manager = SqliteConnectionManager::file(&path).with_init(|c| {
        // Reader pragmas. mmap lets the OS page cache serve reads with no copy.
        c.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA busy_timeout = 5000;
             PRAGMA mmap_size = 1073741824;
             PRAGMA cache_size = -65536;
             PRAGMA temp_store = MEMORY;
             PRAGMA query_only = ON;",
        )
    });
    // 8 readers on 1 vCPU is plenty; more just adds contention.
    Pool::builder()
        .max_size(8)
        .min_idle(Some(4))
        .build(manager)
        .expect("read pool")
});

/// Write connection: SQLite allows exactly one writer. Serialize with a Mutex.
static WRITE_CONN: Lazy<Mutex<Connection>> = Lazy::new(|| {
    let path = env::var("SQLITE_PATH").expect("SQLITE_PATH must be set");
    let c = Connection::open(&path).expect("open write conn");
    c.execute_batch(
        "PRAGMA journal_mode = WAL;
         PRAGMA synchronous = NORMAL;
         PRAGMA busy_timeout = 5000;
         PRAGMA mmap_size = 1073741824;
         PRAGMA cache_size = -65536;
         PRAGMA temp_store = MEMORY;",
    )
    .expect("write pragmas");
    Mutex::new(c)
});

// ---------------------------------------------------------------------------
// SQL (prepared once per connection via rusqlite's statement cache)
// ---------------------------------------------------------------------------

const FEED_SQL: &str = "\
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

const POST_SQL: &str = "\
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
 WHERE p.id = ?1";

const INSERT_POST_SQL: &str =
    "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at";

const INSERT_LIKE_SQL: &str = "\
INSERT INTO likes (user_id, post_id)
SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
ON CONFLICT (user_id, post_id) DO NOTHING";

const POST_EXISTS_SQL: &str = "SELECT 1 FROM posts WHERE id = ?1";

// ---------------------------------------------------------------------------
// JSON output helpers
// ---------------------------------------------------------------------------

#[derive(Serialize)]
struct PostOut {
    id: i64,
    body: String,
    created_at: String,
    author: String,
    like_count: i64,
}

#[derive(Serialize)]
struct FeedOut {
    posts: Vec<PostOut>,
}

#[derive(Serialize)]
struct PostWrap {
    post: PostOut,
}

#[derive(Serialize)]
struct LikeOut {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

#[derive(Serialize)]
struct ErrorOut<'a> {
    error: &'a str,
}

#[derive(Serialize)]
struct HealthOut {
    status: &'static str,
    db: &'static str,
    uptime_s: i64,
}

#[derive(Serialize)]
struct HealthErr<'a> {
    status: &'static str,
    db: &'static str,
    error: &'a str,
}

#[inline]
fn json_ok<T: Serialize>(status: u16, val: &T) -> HttpResponse {
    let bytes = sonic_rs::to_vec(val).expect("serialize");
    HttpResponse::build(actix_web::http::StatusCode::from_u16(status).unwrap())
        .content_type("application/json")
        .body(bytes)
}

#[inline]
fn error(status: u16, msg: &'static str) -> HttpResponse {
    json_ok(status, &ErrorOut { error: msg })
}

// ---------------------------------------------------------------------------
// JWT (hand-rolled HS256, no caching per rule 5)
// ---------------------------------------------------------------------------

static B64: base64::engine::general_purpose::GeneralPurpose =
    base64::engine::general_purpose::URL_SAFE_NO_PAD;

fn b64url_decode(s: &str) -> Option<Vec<u8>> {
    // Reject any character outside the base64url alphabet before decoding.
    if !s
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    {
        return None;
    }
    B64.decode(s).ok()
}

/// Returns Ok((user_id, username)) or Err(()) meaning "401 invalid or expired token".
/// A separate error for "invalid token payload" is handled by the caller.
enum AuthErr {
    InvalidToken,
    InvalidPayload,
}

fn authenticate(req: &HttpRequest) -> Result<(i64, String), AuthErr> {
    let header = req
        .headers()
        .get("authorization")
        .and_then(|h| h.to_str().ok())
        .ok_or(AuthErr::InvalidToken)?;
    let token = header
        .strip_prefix("Bearer ")
        .ok_or(AuthErr::InvalidToken)?;

    let mut parts = token.split('.');
    let head_b64 = parts.next().ok_or(AuthErr::InvalidToken)?;
    let payload_b64 = parts.next().ok_or(AuthErr::InvalidToken)?;
    let sig_b64 = parts.next().ok_or(AuthErr::InvalidToken)?;
    if parts.next().is_some() {
        return Err(AuthErr::InvalidToken);
    }

    // Header must be HS256.
    let head_bytes = b64url_decode(head_b64).ok_or(AuthErr::InvalidToken)?;
    let head: sonic_rs::Value =
        sonic_rs::from_slice(&head_bytes).map_err(|_| AuthErr::InvalidToken)?;
    let alg = head.get("alg").and_then(|v| v.as_str());
    if alg != Some("HS256") {
        return Err(AuthErr::InvalidToken);
    }

    // Verify signature.
    let mut mac = Hmac::<Sha256>::new_from_slice(&JWT_SECRET).map_err(|_| AuthErr::InvalidToken)?;
    mac.update(head_b64.as_bytes());
    mac.update(b".");
    mac.update(payload_b64.as_bytes());
    let expected = mac.finalize().into_bytes();

    let sig = b64url_decode(sig_b64).ok_or(AuthErr::InvalidToken)?;
    if !constant_time_eq(&expected, &sig) {
        return Err(AuthErr::InvalidToken);
    }

    // Decode payload.
    let payload_bytes = b64url_decode(payload_b64).ok_or(AuthErr::InvalidToken)?;
    let payload: sonic_rs::Value =
        sonic_rs::from_slice(&payload_bytes).map_err(|_| AuthErr::InvalidToken)?;

    // exp / nbf checks.
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs() as i64;
    if let Some(exp) = payload.get("exp").and_then(|v| v.as_i64()) {
        if now >= exp {
            return Err(AuthErr::InvalidToken);
        }
    }
    if let Some(nbf) = payload.get("nbf").and_then(|v| v.as_i64()) {
        if now < nbf {
            return Err(AuthErr::InvalidToken);
        }
    }

    // Payload claims.
    let sub = payload.get("sub").and_then(|v| v.as_str());
    let username = payload.get("username").and_then(|v| v.as_str());
    let user_id = sub.and_then(parse_positive_id_str);
    match (user_id, username) {
        (Some(id), Some(name)) => Ok((id, name.to_string())),
        _ => Err(AuthErr::InvalidPayload),
    }
}

#[inline]
fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (x, y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

/// A positive integer in plain decimal digits, else None.
/// Matches the Python reference.
#[inline]
fn parse_positive_id_str(s: &str) -> Option<i64> {
    if s.is_empty() || s.len() > 19 {
        return None;
    }
    let mut n: i64 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    if n > 0 { Some(n) } else { None }
}

/// Path id parser: same rules as the Python reference (ASCII digits, > 0, fits i64).
fn parse_id(raw: &str) -> Option<i64> {
    parse_positive_id_str(raw)
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

async fn health() -> HttpResponse {
    // Quick liveness check via a pooled read connection.
    let pool = READ_POOL.clone();
    let res = web::block(move || {
        let c = pool.get().map_err(|e| e.to_string())?;
        c.query_row("SELECT 1", [], |r| r.get::<_, i64>(0))
            .map_err(|e| e.to_string())
    })
    .await;

    match res {
        Ok(Ok(_)) => {
            let up = START.elapsed().map(|d| d.as_secs() as i64).unwrap_or(0);
            json_ok(
                200,
                &HealthOut {
                    status: "ok",
                    db: "ok",
                    uptime_s: up,
                },
            )
        }
        Ok(Err(e)) => json_ok(
            503,
            &HealthErr {
                status: "degraded",
                db: "unreachable",
                error: &e,
            },
        ),
        Err(_) => json_ok(
            503,
            &HealthErr {
                status: "degraded",
                db: "unreachable",
                error: "join error",
            },
        ),
    }
}
async fn feed() -> HttpResponse {
    let pool = READ_POOL.clone();
    let res = web::block(move || {
        let conn = pool.get().map_err(|e| e.to_string())?;
        let mut stmt = conn.prepare_cached(FEED_SQL).map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([], |row| {
                Ok(PostOut {
                    id: row.get(0)?,
                    body: row.get(1)?,
                    created_at: row.get(2)?,
                    author: row.get(3)?,
                    like_count: row.get(4)?,
                })
            })
            .map_err(|e| e.to_string())?;

        let mut out = Vec::with_capacity(20);
        for r in rows {
            out.push(r.map_err(|e| e.to_string())?);
        }
        Ok::<_, String>(out)
    })
    .await;

    match res {
        Ok(Ok(posts)) => json_ok(200, &FeedOut { posts }),
        _ => error(500, "internal server error"),
    }
}

async fn get_post(path: web::Path<String>) -> HttpResponse {
    let raw = path.into_inner();
    let pid = match parse_id(&raw) {
        Some(p) => p,
        None => return error(400, "invalid post id"),
    };

    let pool = READ_POOL.clone();
    let res = web::block(move || {
        let conn = pool.get().map_err(|e| e.to_string())?;
        let mut stmt = conn.prepare_cached(POST_SQL).map_err(|e| e.to_string())?;
        let row = stmt
            .query_row([pid], |row| {
                Ok(PostOut {
                    id: row.get(0)?,
                    body: row.get(1)?,
                    created_at: row.get(2)?,
                    author: row.get(3)?,
                    like_count: row.get(4)?,
                })
            })
            .optional()
            .map_err(|e| e.to_string())?;
        Ok::<_, String>(row)
    })
    .await;

    match res {
        Ok(Ok(Some(p))) => json_ok(200, &PostWrap { post: p }),
        Ok(Ok(None)) => error(404, "post not found"),
        _ => error(500, "internal server error"),
    }
}

async fn create_post(req: HttpRequest, body: web::Bytes) -> HttpResponse {
    let (user_id, username) = match authenticate(&req) {
        Ok(v) => v,
        Err(AuthErr::InvalidToken) => return error(401, "invalid or expired token"),
        Err(AuthErr::InvalidPayload) => return error(401, "invalid token payload"),
    };

    let parsed: sonic_rs::Value = match sonic_rs::from_slice(&body) {
        Ok(v) => v,
        Err(_) => return error(400, "malformed JSON body"),
    };
    let body_str = match parsed.get("body").and_then(|v| v.as_str()) {
        Some(s) => s,
        None => return error(400, "body is required"),
    };

    // Fast standard trim instead of heavy ECMAScript unicode whitespace matching
    let trimmed = body_str.trim();
    if trimmed.is_empty() {
        return error(400, "body is required");
    }
    if trimmed.chars().count() > 500 {
        return error(400, "body must be at most 500 characters");
    }
    let trimmed_owned = trimmed.to_string();
    let uname = username.clone();

    let res = web::block(move || -> Result<PostOut, String> {
        let conn = WRITE_CONN.lock().map_err(|e| e.to_string())?;
        let mut stmt = conn
            .prepare_cached(INSERT_POST_SQL)
            .map_err(|e| e.to_string())?;
        let (id, created_at) = stmt
            .query_row(rusqlite::params![user_id, trimmed_owned], |r| {
                Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?))
            })
            .map_err(|e| e.to_string())?;

        Ok(PostOut {
            id,
            body: trimmed_owned,
            created_at,
            author: uname,
            like_count: 0,
        })
    })
    .await;

    match res {
        Ok(Ok(p)) => json_ok(201, &PostWrap { post: p }),
        _ => error(500, "internal server error"),
    }
}

async fn like_post(req: HttpRequest, path: web::Path<String>) -> HttpResponse {
    // Auth first, then id validation.
    let (user_id, _username) = match authenticate(&req) {
        Ok(v) => v,
        Err(AuthErr::InvalidToken) => return error(401, "invalid or expired token"),
        Err(AuthErr::InvalidPayload) => return error(401, "invalid token payload"),
    };

    let raw = path.into_inner();
    let pid = match parse_id(&raw) {
        Some(p) => p,
        None => return error(400, "invalid post id"),
    };

    let res = web::block(move || -> Result<(bool, bool), String> {
        let conn = WRITE_CONN.lock().map_err(|e| e.to_string())?;
        {
            let mut stmt = conn
                .prepare_cached(INSERT_LIKE_SQL)
                .map_err(|e| e.to_string())?;
            let inserted = stmt
                .execute(rusqlite::params![user_id, pid])
                .map_err(|e| e.to_string())?;
            if inserted == 1 {
                return Ok((true, false));
            }
        }
        // Nothing inserted: either already liked, or no such post.
        let exists: Option<i64> = conn
            .query_row(POST_EXISTS_SQL, [pid], |r| r.get(0))
            .optional()
            .map_err(|e| e.to_string())?;
        if exists.is_some() {
            Ok((true, true))
        } else {
            Ok((false, false)) // sentinel: post not found
        }
    })
    .await;

    match res {
        Ok(Ok((true, false))) => json_ok(
            201,
            &LikeOut {
                liked: true,
                already_liked: false,
                post_id: pid,
            },
        ),
        Ok(Ok((true, true))) => json_ok(
            200,
            &LikeOut {
                liked: true,
                already_liked: true,
                post_id: pid,
            },
        ),
        Ok(Ok((false, false))) => error(404, "post not found"),
        _ => error(500, "internal server error"),
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    // Warm the pools and the write connection.
    Lazy::force(&READ_POOL);
    Lazy::force(&WRITE_CONN);
    Lazy::force(&JWT_SECRET);

    let cpus = available_parallelism().map(|n| n.get()).unwrap_or(1);
    let host = env::var("HOST").unwrap_or_else(|_| "127.0.0.1".into());
    let port: u16 = env::var("PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(3000);

    eprintln!("listening on {host}:{port}, workers={}", cpus);

    HttpServer::new(|| {
        App::new()
            .route("/health", web::get().to(health))
            .route("/feed", web::get().to(feed))
            .route("/posts", web::post().to(create_post))
            .route("/posts/{id}", web::get().to(get_post))
            .route("/posts/{id}/like", web::post().to(like_post))
            .default_service(web::to(not_found))
    })
    .workers(cpus)
    .keep_alive(Duration::from_secs(75))
    .client_request_timeout(Duration::from_secs(30))
    .backlog(4096)
    .bind((host.as_str(), port))?
    .run()
    .await
}

async fn not_found() -> HttpResponse {
    error(404, "not found")
}
