//! Baseline entry: axum + rusqlite, the Rust counterpart of go-nethttp-DblD.
//!
//! Shape: a small pool of read connections and one write connection (SQLite allows one
//! writer at a time; one connection serializes writes instead of retrying on SQLITE_BUSY).
//! SQLite calls are blocking, so each runs on tokio's blocking pool. Statements use
//! rusqlite's prepared-statement cache (plans only, no data: rule 5).

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use axum::body::Bytes;
use axum::extract::{Path, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::Router;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use hmac::{Hmac, KeyInit, Mac};
use rusqlite::{params, Connection, OptionalExtension};
use serde::Serialize;
use serde_json::Value;
use sha2::Sha256;

const READERS: usize = 4;

const POST_COLS: &str = "SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id ";

#[derive(Serialize)]
struct Post {
    id: i64,
    body: String,
    created_at: String,
    author: String,
    like_count: i64,
}

struct App {
    started: Instant,
    secret: Vec<u8>,
    readers: Vec<Mutex<Connection>>,
    next: AtomicUsize,
    writer: Mutex<Connection>,
}

type Shared = Arc<App>;

fn main() {
    let path = env("SQLITE_PATH");
    let app = Arc::new(App {
        started: Instant::now(),
        secret: env("JWT_SECRET").into_bytes(),
        readers: (0..READERS).map(|_| Mutex::new(open(&path))).collect(),
        next: AtomicUsize::new(0),
        writer: Mutex::new(open(&path)),
    });

    let router = Router::new()
        .route("/health", get(health))
        .route("/feed", get(feed))
        .route("/posts", post(create))
        .route("/posts/{id}", get(get_post))
        .route("/posts/{id}/like", post(like))
        .fallback(|| async { fail(StatusCode::NOT_FOUND, "not found") })
        .with_state(app);

    let addr = format!("{}:{}", env("HOST"), env("PORT"));
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("tokio runtime")
        .block_on(async move {
            let listener = tokio::net::TcpListener::bind(&addr).await.expect("bind");
            eprintln!("listening on {addr}");
            // hyper keeps keep-alive connections open until the client closes them, which
            // covers the spec's ">= 65 s idle" for Nginx's upstream pool.
            axum::serve(listener, router).await.expect("serve");
        });
}

fn open(path: &str) -> Connection {
    let c = Connection::open(path).expect("open sqlite");
    // synchronous=NORMAL in WAL mode is the weakest setting rule 6 allows.
    c.execute_batch("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;")
        .expect("pragmas");
    c
}

impl App {
    /// Runs `f` on a read connection, on the blocking pool.
    async fn read<T: Send + 'static>(
        self: &Shared,
        f: impl FnOnce(&Connection) -> rusqlite::Result<T> + Send + 'static,
    ) -> rusqlite::Result<T> {
        let app = self.clone();
        tokio::task::spawn_blocking(move || {
            let i = app.next.fetch_add(1, Ordering::Relaxed) % READERS;
            f(&app.readers[i].lock().unwrap())
        })
        .await
        .expect("blocking task")
    }

    /// Runs `f` on the single write connection, on the blocking pool.
    async fn write<T: Send + 'static>(
        self: &Shared,
        f: impl FnOnce(&Connection) -> rusqlite::Result<T> + Send + 'static,
    ) -> rusqlite::Result<T> {
        let app = self.clone();
        tokio::task::spawn_blocking(move || f(&app.writer.lock().unwrap()))
            .await
            .expect("blocking task")
    }
}

fn row_to_post(r: &rusqlite::Row) -> rusqlite::Result<Post> {
    Ok(Post { id: r.get(0)?, body: r.get(1)?, created_at: r.get(2)?, author: r.get(3)?, like_count: r.get(4)? })
}

async fn health(State(app): State<Shared>) -> Response {
    let up = app.started.elapsed().as_secs();
    match app.read(|c| c.query_row("SELECT 1", [], |r| r.get::<_, i64>(0))).await {
        Ok(_) => raw(StatusCode::OK, format!(r#"{{"status":"ok","db":"ok","uptime_s":{up}}}"#)),
        Err(e) => json(
            StatusCode::SERVICE_UNAVAILABLE,
            &serde_json::json!({"status": "degraded", "db": "unreachable", "error": e.to_string()}),
        ),
    }
}

async fn feed(State(app): State<Shared>) -> Response {
    let res = app
        .read(|c| {
            let mut st = c.prepare_cached(&format!("{POST_COLS}ORDER BY p.created_at DESC, p.id DESC LIMIT 20"))?;
            st.query_map([], row_to_post)?.collect::<rusqlite::Result<Vec<_>>>()
        })
        .await;
    #[derive(Serialize)]
    struct Out {
        posts: Vec<Post>,
    }
    match res {
        Ok(posts) => json(StatusCode::OK, &Out { posts }),
        Err(e) => internal(e),
    }
}

#[derive(Serialize)]
struct OnePost {
    post: Post,
}

async fn get_post(State(app): State<Shared>, Path(raw_id): Path<String>) -> Response {
    let Some(id) = parse_id(&raw_id) else { return fail(StatusCode::BAD_REQUEST, "invalid post id") };
    let res = app
        .read(move |c| {
            c.prepare_cached(&format!("{POST_COLS}WHERE p.id = ?"))?.query_row([id], row_to_post).optional()
        })
        .await;
    match res {
        Ok(Some(post)) => json(StatusCode::OK, &OnePost { post }),
        Ok(None) => fail(StatusCode::NOT_FOUND, "post not found"),
        Err(e) => internal(e),
    }
}

async fn create(State(app): State<Shared>, headers: HeaderMap, body: Bytes) -> Response {
    let (uid, author) = match auth(&app, &headers) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let Ok(v) = serde_json::from_slice::<Value>(&body) else {
        return fail(StatusCode::BAD_REQUEST, "malformed JSON body");
    };
    let text = v.get("body").and_then(Value::as_str).unwrap_or("").trim().to_owned();
    if text.is_empty() {
        return fail(StatusCode::BAD_REQUEST, "body is required");
    }
    if text.chars().count() > 500 {
        // characters, as SQLite's length() counts them
        return fail(StatusCode::BAD_REQUEST, "body must be at most 500 characters");
    }
    let t = text.clone();
    let res = app
        .write(move |c| {
            c.prepare_cached("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")?
                .query_row(params![uid, t], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?)))
        })
        .await;
    match res {
        Ok((id, created_at)) => {
            json(StatusCode::CREATED, &OnePost { post: Post { id, body: text, created_at, author, like_count: 0 } })
        }
        Err(e) => internal(e),
    }
}

async fn like(State(app): State<Shared>, headers: HeaderMap, Path(raw_id): Path<String>) -> Response {
    // auth before id, per spec
    let (uid, _) = match auth(&app, &headers) {
        Ok(v) => v,
        Err(r) => return r,
    };
    let Some(id) = parse_id(&raw_id) else { return fail(StatusCode::BAD_REQUEST, "invalid post id") };
    let exists = app
        .read(move |c| c.prepare_cached("SELECT 1 FROM posts WHERE id = ?")?.query_row([id], |_| Ok(())).optional())
        .await;
    match exists {
        Ok(Some(())) => {}
        Ok(None) => return fail(StatusCode::NOT_FOUND, "post not found"),
        Err(e) => return internal(e),
    }
    let res = app
        .write(move |c| {
            c.prepare_cached(
                "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
            )?
            .execute(params![uid, id])
        })
        .await;
    #[derive(Serialize)]
    struct Out {
        liked: bool,
        already_liked: bool,
        post_id: i64,
    }
    match res {
        Ok(n) => {
            let status = if n == 1 { StatusCode::CREATED } else { StatusCode::OK };
            json(status, &Out { liked: true, already_liked: n == 0, post_id: id })
        }
        Err(e) => internal(e),
    }
}

/// Verifies an HS256 bearer token and returns (user id, username), or the 401 to send.
fn auth(app: &App, headers: &HeaderMap) -> Result<(i64, String), Response> {
    let tok = headers
        .get(header::AUTHORIZATION)
        .and_then(|h| h.to_str().ok())
        .and_then(|h| h.strip_prefix("Bearer "))
        .ok_or_else(|| fail(StatusCode::UNAUTHORIZED, "missing bearer token"))?;
    let claims = verify(&app.secret, tok).ok_or_else(|| fail(StatusCode::UNAUTHORIZED, "invalid or expired token"))?;
    let uid = claims.get("sub").and_then(Value::as_str).and_then(parse_id);
    let name = claims.get("username").and_then(Value::as_str);
    match (uid, name) {
        (Some(uid), Some(name)) => Ok((uid, name.to_owned())),
        _ => Err(fail(StatusCode::UNAUTHORIZED, "invalid token payload")),
    }
}

fn verify(secret: &[u8], tok: &str) -> Option<Value> {
    let mut parts = tok.split('.');
    let (h, p, s) = (parts.next()?, parts.next()?, parts.next()?);
    if parts.next().is_some() {
        return None;
    }
    let hdr: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(h).ok()?).ok()?;
    if hdr.get("alg").and_then(Value::as_str) != Some("HS256") {
        return None;
    }
    let sig = URL_SAFE_NO_PAD.decode(s).ok()?;
    let mut mac = Hmac::<Sha256>::new_from_slice(secret).ok()?;
    mac.update(h.as_bytes());
    mac.update(b".");
    mac.update(p.as_bytes());
    mac.verify_slice(&sig).ok()?; // constant-time compare
    let claims: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(p).ok()?).ok()?;
    let exp = claims.get("exp").and_then(Value::as_f64)?;
    let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_secs_f64();
    (now < exp).then_some(claims)
}

/// Accepts only a positive base-10 integer: no sign, no fraction, no spaces.
fn parse_id(s: &str) -> Option<i64> {
    match s.as_bytes().first() {
        Some(b'1'..=b'9') if s.bytes().all(|b| b.is_ascii_digit()) => s.parse().ok(),
        _ => None,
    }
}

fn json<T: Serialize>(status: StatusCode, v: &T) -> Response {
    match serde_json::to_string(v) {
        Ok(s) => raw(status, s),
        Err(e) => internal(e),
    }
}

fn raw(status: StatusCode, body: String) -> Response {
    (status, [(header::CONTENT_TYPE, HeaderValue::from_static("application/json"))], body).into_response()
}

fn fail(status: StatusCode, msg: &str) -> Response {
    json(status, &serde_json::json!({ "error": msg }))
}

fn internal(e: impl std::fmt::Display) -> Response {
    eprintln!("internal: {e}");
    fail(StatusCode::INTERNAL_SERVER_ERROR, "internal server error")
}

fn env(k: &str) -> String {
    std::env::var(k).unwrap_or_else(|_| panic!("{k} is not set"))
}
