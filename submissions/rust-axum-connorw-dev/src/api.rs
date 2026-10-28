//! The HTTP API from SPEC.md: routes, authentication, validation and response shapes.

use std::{
    sync::Arc,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use axum::{
    Router,
    body::Bytes,
    extract::{DefaultBodyLimit, FromRequestParts, Path, State, rejection::BytesRejection},
    http::{StatusCode, header, request::Parts},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD as B64};
use hmac::{Hmac, Mac};
use serde::Serialize;
use serde_json::{Value, json};
use sha2::Sha256;
use tower_http::timeout::RequestBodyTimeoutLayer;

use crate::db::{self, Db, Like, Post};

pub struct App {
    pub db: Db,
    pub jwt_key: Hmac<Sha256>, // keyed once at startup; every token's signature is still checked
    pub started: Instant,
}

type AppState = State<Arc<App>>;

pub fn router(app: Arc<App>) -> Router {
    // Only creating a post reads a body: cap it at 16 KiB, and give a slow upload 10 s.
    let upload_limits = (DefaultBodyLimit::max(16 * 1024), RequestBodyTimeoutLayer::new(Duration::from_secs(10)));
    Router::new()
        .route("/health", get(health))
        .route("/feed", get(feed))
        .route("/posts", post(create_post).layer(upload_limits))
        .route("/posts/{id}", get(get_post))
        .route("/posts/{id}/like", post(like_post))
        .fallback(|| async { ApiError(StatusCode::NOT_FOUND, "not found") })
        .with_state(app)
}

#[derive(Serialize)]
struct Healthy {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct Degraded {
    status: &'static str,
    db: &'static str,
    error: String,
}

async fn health(State(app): AppState) -> Response {
    match app.db.check() {
        Ok(()) => Json(Healthy { status: "ok", db: "ok", uptime_s: app.started.elapsed().as_secs() }).into_response(),
        Err(e) => {
            let body = Degraded { status: "degraded", db: "unreachable", error: e.to_string() };
            (StatusCode::SERVICE_UNAVAILABLE, Json(body)).into_response()
        }
    }
}

async fn feed(State(app): AppState) -> Result<Response, ApiError> {
    let json = app.db.read(db::feed)?; // already serialized, straight from SQLite's buffers
    Ok(([(header::CONTENT_TYPE, "application/json")], json).into_response())
}

#[derive(Serialize)]
struct OnePost {
    post: Post,
}

async fn get_post(State(app): AppState, Path(id): Path<String>) -> Result<Json<OnePost>, ApiError> {
    let id = post_id(&id)?;
    let post = app.db.read(|db| db::post(db, id))?.ok_or(ApiError(StatusCode::NOT_FOUND, "post not found"))?;
    Ok(Json(OnePost { post }))
}

async fn create_post(
    State(app): AppState,
    user: User,
    body: Result<Bytes, BytesRejection>,
) -> Result<(StatusCode, Json<OnePost>), ApiError> {
    let bad_request = |message| ApiError(StatusCode::BAD_REQUEST, message);
    let body = body.map_err(|e| match e.status() {
        StatusCode::PAYLOAD_TOO_LARGE => ApiError(StatusCode::PAYLOAD_TOO_LARGE, "body too large"),
        _ => bad_request("malformed JSON body"), // the upload timed out or broke off
    })?;
    let input: Value = serde_json::from_slice(&body).map_err(|_| bad_request("malformed JSON body"))?;
    let text = input.get("body").and_then(Value::as_str).map(trim_js).unwrap_or_default();
    if text.is_empty() {
        return Err(bad_request("body is required"));
    }
    // Counted in code points, like the schema's CHECK (length(body) BETWEEN 1 AND 500).
    if text.chars().count() > 500 {
        return Err(bad_request("body must be at most 500 characters"));
    }
    let text = text.to_owned();
    let post = app.db.write(move |db| db::create_post(db, user.id, user.name, text)).await?;
    Ok((StatusCode::CREATED, Json(OnePost { post })))
}

#[derive(Serialize)]
struct Liked {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

// `User` is extracted before `Path`, so a bad token is a 401 even when the id is also invalid.
async fn like_post(
    State(app): AppState,
    user: User,
    Path(id): Path<String>,
) -> Result<(StatusCode, Json<Liked>), ApiError> {
    let post_id = post_id(&id)?;
    let (status, already_liked) = match app.db.write(move |db| db::like(db, user.id, post_id)).await? {
        Like::Added => (StatusCode::CREATED, false),
        Like::AlreadyLiked => (StatusCode::OK, true),
        Like::NoSuchPost => return Err(ApiError(StatusCode::NOT_FOUND, "post not found")),
    };
    Ok((status, Json(Liked { liked: true, already_liked, post_id })))
}

/// A JSON response, serialized into a plain `Vec`. axum's own `Json` (behind its `json` feature)
/// writes through a `BytesMut`, which profiled several microseconds slower on a full feed.
struct Json<T>(T);

impl<T: Serialize> IntoResponse for Json<T> {
    fn into_response(self) -> Response {
        match serde_json::to_vec(&self.0) {
            Ok(body) => ([(header::CONTENT_TYPE, "application/json")], body).into_response(),
            Err(_) => StatusCode::INTERNAL_SERVER_ERROR.into_response(),
        }
    }
}

/// An error response: the status and the message sent as `{"error": message}`.
pub struct ApiError(StatusCode, &'static str);

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.0, Json(json!({ "error": self.1 }))).into_response()
    }
}

impl From<rusqlite::Error> for ApiError {
    fn from(e: rusqlite::Error) -> Self {
        // The only foreign key a write can break is its user's: a validly signed token for a user id
        // that isn't in `users`. Writing an orphan row would leave a post the feed never shows.
        if e.sqlite_error().is_some_and(|e| e.extended_code == rusqlite::ffi::SQLITE_CONSTRAINT_FOREIGNKEY) {
            return ApiError(StatusCode::UNAUTHORIZED, "invalid token payload");
        }
        eprintln!("database: {e}");
        ApiError(StatusCode::INTERNAL_SERVER_ERROR, "internal server error")
    }
}

/// The caller, from `Authorization: Bearer <JWT>`.
pub struct User {
    id: i64,
    name: String,
}

impl FromRequestParts<Arc<App>> for User {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, app: &Arc<App>) -> Result<User, ApiError> {
        let unauthorized = |message| ApiError(StatusCode::UNAUTHORIZED, message);
        let header = parts.headers.get(header::AUTHORIZATION);
        let token = header.and_then(|value| value.as_bytes().strip_prefix(b"Bearer "));
        let claims = verify_jwt(&app.jwt_key, token.ok_or(unauthorized("missing bearer token"))?)
            .ok_or(unauthorized("invalid or expired token"))?;
        let id = claims.get("sub").and_then(Value::as_str).and_then(positive_int);
        match (id, claims.get("username").and_then(Value::as_str)) {
            (Some(id), Some(name)) => Ok(User { id, name: name.to_owned() }),
            _ => Err(unauthorized("invalid token payload")),
        }
    }
}

/// HS256 only: checks the signature in constant time, then the `alg` header and `exp`.
fn verify_jwt(key: &Hmac<Sha256>, token: &[u8]) -> Option<serde_json::Map<String, Value>> {
    let dot = token.iter().rposition(|&b| b == b'.')?;
    let (signed, signature) = (&token[..dot], &token[dot + 1..]);
    let mut mac = key.clone();
    mac.update(signed);
    mac.verify_slice(&B64.decode(signature).ok()?).ok()?;
    let (header, payload) = signed.split_at(signed.iter().position(|&b| b == b'.')?);
    let header: Value = serde_json::from_slice(&B64.decode(header).ok()?).ok()?;
    if header.get("alg")?.as_str()? != "HS256" {
        return None;
    }
    let Value::Object(claims) = serde_json::from_slice(&B64.decode(&payload[1..]).ok()?).ok()? else {
        return None;
    };
    let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_secs_f64();
    (claims.get("exp")?.as_f64()? > now).then_some(claims)
}

fn post_id(raw: &str) -> Result<i64, ApiError> {
    positive_int(raw).ok_or(ApiError(StatusCode::BAD_REQUEST, "invalid post id"))
}

/// Plain decimal digits and greater than zero: rejects "0", "-1", "+1", "1.5" and "abc".
fn positive_int(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok().filter(|&n| n > 0)
}

/// The whitespace set of JavaScript's `String.prototype.trim()`, which the reference servers used.
fn trim_js(s: &str) -> &str {
    s.trim_matches(|c: char| (c.is_whitespace() && c != '\u{85}') || c == '\u{feff}')
}

#[cfg(test)]
mod tests {
    //! The real router against a small temporary database, driven without a network.
    use super::*;
    use crate::db::tests::TempDb;
    use axum::{body::Body, http::Request};
    use hmac::KeyInit;
    use tower::ServiceExt;

    /// A router on a fresh database file, deleted when the guard drops.
    fn app() -> (Router, Hmac<Sha256>, TempDb) {
        let file = TempDb::new();
        let key = Hmac::new_from_slice(b"test secret").unwrap();
        let app = App { db: Db::open(file.path()).unwrap(), jwt_key: key.clone(), started: Instant::now() };
        (router(Arc::new(app)), key, file)
    }

    async fn send(app: &Router, method: &str, uri: &str, auth: Option<&str>, body: &str) -> (u16, String) {
        let mut request = Request::builder().method(method).uri(uri);
        if let Some(auth) = auth {
            request = request.header(header::AUTHORIZATION, auth);
        }
        let response = app.clone().oneshot(request.body(Body::from(body.to_owned())).unwrap()).await.unwrap();
        let status = response.status().as_u16();
        let bytes = axum::body::to_bytes(response.into_body(), usize::MAX).await.unwrap();
        (status, String::from_utf8(bytes.to_vec()).unwrap())
    }

    fn sign(key: &Hmac<Sha256>, header: &str, claims: &str) -> String {
        let signed = format!("{}.{}", B64.encode(header), B64.encode(claims));
        let mut mac = key.clone();
        mac.update(signed.as_bytes());
        format!("{signed}.{}", B64.encode(mac.finalize().into_bytes()))
    }

    fn bearer(key: &Hmac<Sha256>, claims: &str) -> String {
        format!("Bearer {}", sign(key, r#"{"alg":"HS256","typ":"JWT"}"#, claims))
    }

    fn ann(key: &Hmac<Sha256>) -> String {
        let exp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 60;
        bearer(key, &format!(r#"{{"sub":"1","username":"ann","exp":{exp}}}"#))
    }

    #[tokio::test]
    async fn reads() {
        let (app, _, _file) = app();
        let first = r#"{"id":1,"body":"first","created_at":"2026-01-01T00:00:00.000Z","author":"ann","like_count":1}"#;
        let second = r#"{"id":2,"body":"say \"hi\" \\ bye","created_at":"2026-01-02T00:00:00.000Z","author":"bob","like_count":0}"#;
        assert_eq!(send(&app, "GET", "/posts/1", None, "").await, (200, format!(r#"{{"post":{first}}}"#)));
        assert_eq!(send(&app, "GET", "/feed", None, "").await, (200, format!(r#"{{"posts":[{second},{first}]}}"#)));
        assert_eq!(send(&app, "GET", "/posts/9", None, "").await, (404, r#"{"error":"post not found"}"#.into()));
        for id in ["0", "-1", "abc", "1.5"] {
            let invalid = (400, r#"{"error":"invalid post id"}"#.into());
            assert_eq!(send(&app, "GET", &format!("/posts/{id}"), None, "").await, invalid, "{id}");
        }
        assert_eq!(send(&app, "GET", "/nope", None, "").await, (404, r#"{"error":"not found"}"#.into()));
        let (status, health) = send(&app, "GET", "/health", None, "").await;
        assert!(status == 200 && health.starts_with(r#"{"status":"ok","db":"ok","uptime_s":"#), "{health}");
    }

    #[tokio::test]
    async fn writes_are_visible_once_acknowledged() {
        let (app, key, _file) = app();
        let ann = ann(&key);
        let (status, created) = send(&app, "POST", "/posts", Some(&ann), r#"{"body":"  hello  "}"#).await;
        assert_eq!(status, 201, "{created}");
        let post: Value = serde_json::from_str(&created).unwrap();
        assert_eq!(
            (&post["post"]["body"], &post["post"]["author"], &post["post"]["like_count"]),
            (&json!("hello"), &json!("ann"), &json!(0))
        );
        let id = post["post"]["id"].as_i64().unwrap();
        let liked = |already: bool| format!(r#"{{"liked":true,"already_liked":{already},"post_id":{id}}}"#);
        assert_eq!(send(&app, "POST", &format!("/posts/{id}/like"), Some(&ann), "").await, (201, liked(false)));
        assert_eq!(send(&app, "POST", &format!("/posts/{id}/like"), Some(&ann), "").await, (200, liked(true)));
        let (_, feed) = send(&app, "GET", "/feed", None, "").await;
        let top = &serde_json::from_str::<Value>(&feed).unwrap()["posts"][0];
        assert_eq!((&top["id"], &top["like_count"]), (&json!(id), &json!(1)));
        let missing = (404, r#"{"error":"post not found"}"#.into());
        assert_eq!(send(&app, "POST", "/posts/9/like", Some(&ann), "").await, missing);
    }

    #[tokio::test]
    async fn rejects_bad_requests() {
        let (app, key, _file) = app();
        let error = |status: u16, message: &str| (status, format!(r#"{{"error":"{message}"}}"#));
        let exp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 60;
        let no_such_user = bearer(&key, &format!(r#"{{"sub":"999","username":"ghost","exp":{exp}}}"#));
        let cases = [
            (Some(no_such_user.clone()), "/posts", r#"{"body":"hi"}"#, error(401, "invalid token payload")),
            (Some(no_such_user), "/posts/1/like", "", error(401, "invalid token payload")),
            (None, "/posts", "{}", error(401, "missing bearer token")),
            (Some("Basic abc".to_owned()), "/posts", "{}", error(401, "missing bearer token")),
            (Some("Bearer abc".to_owned()), "/posts", "{}", error(401, "invalid or expired token")),
            (
                Some(bearer(&key, &format!(r#"{{"sub":"x","username":"ann","exp":{exp}}}"#))),
                "/posts",
                "{}",
                error(401, "invalid token payload"),
            ),
            (None, "/posts/abc/like", "", error(401, "missing bearer token")),
            (Some(ann(&key)), "/posts/abc/like", "", error(400, "invalid post id")),
            (Some(ann(&key)), "/posts", "{bad json", error(400, "malformed JSON body")),
            (Some(ann(&key)), "/posts", r#"{"body":"   "}"#, error(400, "body is required")),
            (Some(ann(&key)), "/posts", r#"{"body":123}"#, error(400, "body is required")),
            (
                Some(ann(&key)),
                "/posts",
                &format!(r#"{{"body":"{}"}}"#, "a".repeat(501)),
                error(400, "body must be at most 500 characters"),
            ),
        ];
        for (auth, uri, body, expected) in cases {
            assert_eq!(send(&app, "POST", uri, auth.as_deref(), body).await, expected, "{uri} {body}");
        }
    }

    #[test]
    fn jwt() {
        let key = Hmac::new_from_slice(b"secret").unwrap();
        let hs256 = r#"{"alg":"HS256","typ":"JWT"}"#;
        let exp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs() + 60;
        let claims = format!(r#"{{"sub":"7","username":"ann","exp":{exp}}}"#);
        let token = sign(&key, hs256, &claims);
        assert_eq!(verify_jwt(&key, token.as_bytes()).unwrap()["username"], "ann");
        let other_key = Hmac::new_from_slice(b"other").unwrap();
        assert!(verify_jwt(&key, sign(&other_key, hs256, &claims).as_bytes()).is_none());
        assert!(verify_jwt(&key, sign(&key, r#"{"alg":"none"}"#, &claims).as_bytes()).is_none());
        assert!(verify_jwt(&key, sign(&key, r#"{"alg":"HS512"}"#, &claims).as_bytes()).is_none());
        assert!(verify_jwt(&key, sign(&key, hs256, r#"{"sub":"7","username":"ann","exp":1}"#).as_bytes()).is_none());
        assert!(verify_jwt(&key, sign(&key, hs256, r#"{"sub":"7","username":"ann"}"#).as_bytes()).is_none());
        for garbage in ["", "abc", "a.b", "a.b.c", "..", &format!("{token}x"), &format!("x{token}")] {
            assert!(verify_jwt(&key, garbage.as_bytes()).is_none(), "{garbage}");
        }
    }

    #[test]
    fn ids_and_trimming() {
        assert_eq!((positive_int("42"), positive_int("007")), (Some(42), Some(7)));
        for bad in ["", "0", "-1", "+1", "1.5", "abc", " 1", "1e3", "99999999999999999999"] {
            assert_eq!(positive_int(bad), None, "{bad}");
        }
        assert_eq!(trim_js("\u{feff}\u{a0} hi \t\n"), "hi");
        assert_eq!(trim_js("\u{85}x\u{85}"), "\u{85}x\u{85}"); // NEL is not whitespace in JavaScript
    }
}
