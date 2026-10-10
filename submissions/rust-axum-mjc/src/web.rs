use crate::{
    auth::{Auth, positive_id},
    db::{Db, DbError},
};
use axum::{
    Json, Router,
    body::Bytes,
    extract::{Path, State, rejection::PathRejection},
    http::{HeaderMap, StatusCode, header::CONTENT_TYPE},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use serde::{Deserialize, Serialize};
use serde_json::value::RawValue;
use std::{borrow::Cow, sync::Arc, time::Instant};

struct AppState {
    db: Db,
    auth: Auth,
    started: Instant,
}

pub fn router(db: Db, auth: Auth) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/feed", get(feed))
        .route("/posts", post(create_post))
        .route("/posts/{id}", get(get_post))
        .route("/posts/{id}/like", post(like_post))
        .fallback(|| async { error(StatusCode::NOT_FOUND, "not found") })
        .with_state(Arc::new(AppState {
            db,
            auth,
            started: Instant::now(),
        }))
}

#[derive(Serialize)]
struct ErrorBody<'a> {
    error: &'a str,
}

fn error(status: StatusCode, message: &str) -> Response {
    (status, Json(ErrorBody { error: message })).into_response()
}

fn encoded_json(status: StatusCode, body: Vec<u8>) -> Response {
    (status, [(CONTENT_TYPE, "application/json")], body).into_response()
}

fn internal_error(error_value: DbError) -> Response {
    eprintln!("database request failed: {error_value}");
    error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error")
}

#[derive(Serialize)]
struct HealthBody {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct DegradedHealth {
    status: &'static str,
    db: &'static str,
    error: String,
}

async fn health(State(state): State<Arc<AppState>>) -> Response {
    match state.db.health().await {
        Ok(()) => Json(HealthBody {
            status: "ok",
            db: "ok",
            uptime_s: state.started.elapsed().as_secs(),
        })
        .into_response(),
        Err(error_value) => (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(DegradedHealth {
                status: "degraded",
                db: "unreachable",
                error: error_value.to_string(),
            }),
        )
            .into_response(),
    }
}

async fn feed(State(state): State<Arc<AppState>>) -> Response {
    match state.db.feed().await {
        Ok(body) => encoded_json(StatusCode::OK, body),
        Err(error_value) => internal_error(error_value),
    }
}

async fn get_post(
    State(state): State<Arc<AppState>>,
    path: Result<Path<String>, PathRejection>,
) -> Response {
    let Some(id) = path.ok().and_then(|Path(id)| positive_id(&id)) else {
        return error(StatusCode::BAD_REQUEST, "invalid post id");
    };
    match state.db.post(id).await {
        Ok(Some(body)) => encoded_json(StatusCode::OK, body),
        Ok(None) => error(StatusCode::NOT_FOUND, "post not found"),
        Err(error_value) => internal_error(error_value),
    }
}

#[derive(Deserialize)]
struct Incoming<'a> {
    // A direct Cow field lets serde borrow unescaped JSON strings. Missing body
    // defaults to an empty string, which follows the same required-body check.
    #[serde(default, borrow)]
    body: Cow<'a, str>,
}

fn post_body(bytes: &Bytes) -> Result<Bytes, &'static str> {
    let object_start = bytes.iter().position(|byte| !byte.is_ascii_whitespace());
    if !object_start.is_some_and(|index| bytes[index] == b'{') {
        let _: &RawValue = serde_json::from_slice(bytes).map_err(|_| "malformed JSON body")?;
        return Err("body is required");
    }
    // Retain borrowed strings and parse accepted requests once. Rejected
    // type errors get a full-document validation pass to preserve error order.
    let incoming: Incoming<'_> = match serde_json::from_slice(bytes) {
        Ok(incoming) => incoming,
        Err(error) if error.is_data() => {
            // Deserialization can stop at a type error before reaching invalid
            // trailing JSON. Validate those uncommon rejected requests fully
            // to preserve malformed-JSON precedence.
            let _: &RawValue = serde_json::from_slice(bytes).map_err(|_| "malformed JSON body")?;
            return Err("body is required");
        }
        Err(_) => return Err("malformed JSON body"),
    };
    let trimmed = incoming.body.trim();
    if trimmed.is_empty() {
        return Err("body is required");
    }
    if trimmed.chars().count() > 500 {
        return Err("body must be at most 500 characters");
    }
    let start = incoming.body.len() - incoming.body.trim_start().len();
    let end = start + trimmed.len();
    Ok(match incoming.body {
        Cow::Borrowed(text) => bytes.slice_ref(text.trim().as_bytes()),
        // Escapes require decoding; keep that allocation and slice its padding.
        Cow::Owned(text) => Bytes::from(text).slice(start..end),
    })
}

async fn create_post(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    bytes: Bytes,
) -> Response {
    let user = match state.auth.authenticate(&headers) {
        Ok(user) => user,
        Err(message) => return error(StatusCode::UNAUTHORIZED, message),
    };
    let body = match post_body(&bytes) {
        Ok(body) => body,
        Err(message) => return error(StatusCode::BAD_REQUEST, message),
    };
    match state
        .db
        .create_post(user.user_id, body, user.username)
        .await
    {
        Ok(body) => encoded_json(StatusCode::CREATED, body),
        Err(error_value) => internal_error(error_value),
    }
}

#[derive(Serialize)]
struct LikeBody {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

async fn like_post(
    State(state): State<Arc<AppState>>,
    path: Result<Path<String>, PathRejection>,
    headers: HeaderMap,
) -> Response {
    let user_id = match state.auth.authenticate_id(&headers) {
        Ok(user) => user,
        Err(message) => return error(StatusCode::UNAUTHORIZED, message),
    };
    let Some(post_id) = path.ok().and_then(|Path(id)| positive_id(&id)) else {
        return error(StatusCode::BAD_REQUEST, "invalid post id");
    };
    match state.db.like(user_id, post_id).await {
        Ok(Some(already_liked)) => (
            if already_liked {
                StatusCode::OK
            } else {
                StatusCode::CREATED
            },
            Json(LikeBody {
                liked: true,
                already_liked,
                post_id,
            }),
        )
            .into_response(),
        Ok(None) => error(StatusCode::NOT_FOUND, "post not found"),
        Err(error_value) => internal_error(error_value),
    }
}

#[cfg(test)]
include!("tests/web.rs");
