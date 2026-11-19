use std::sync::Arc;

use axum::{
    body::Bytes,
    extract::{Path, State},
    http::{StatusCode, header},
    response::{IntoResponse, Response},
};
use serde_json::Value;

use crate::{AppState, auth::AuthUser, db, json};

fn json_response(status: StatusCode, body: Vec<u8>) -> Response {
    (status, [(header::CONTENT_TYPE, "application/json")], body).into_response()
}

pub fn json_err(status: StatusCode, message: &'static str) -> Response {
    json_response(status, json::error(message))
}

pub async fn feed(State(state): State<Arc<AppState>>) -> Response {
    match state.db.feed().await {
        Ok(body) => json_response(StatusCode::OK, body),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn health(State(state): State<Arc<AppState>>) -> Response {
    let uptime = state.start_time.elapsed().as_secs();

    match state.db.ping().await {
        Ok(()) => json_response(StatusCode::OK, json::health_ok(uptime)),
        Err(error) => json_response(
            StatusCode::SERVICE_UNAVAILABLE,
            json::health_degraded(&error),
        ),
    }
}

pub fn parse_positive_id(s: &str) -> Option<i64> {
    let b = s.as_bytes();
    if b.is_empty() || !(b'1'..=b'9').contains(&b[0]) || !b[1..].iter().all(u8::is_ascii_digit) {
        return None;
    }
    s.parse::<i64>().ok()
}

pub async fn get_post(
    Path(post_id): Path<String>,
    State(state): State<Arc<AppState>>,
) -> Response {
    let Some(id) = parse_positive_id(&post_id) else {
        return json_err(StatusCode::BAD_REQUEST, "invalid post id");
    };

    match state.db.get_post(id).await {
        Ok(Some(body)) => json_response(StatusCode::OK, body),
        Ok(None) => json_err(StatusCode::NOT_FOUND, "post not found"),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn like_post(
    user: AuthUser,
    Path(post_id): Path<String>,
    State(state): State<Arc<AppState>>,
) -> Response {
    let Some(id) = parse_positive_id(&post_id) else {
        return json_err(StatusCode::BAD_REQUEST, "invalid post id");
    };

    match state.db.like(user.user_id, id).await {
        Ok(db::LikeOutcome::Created(body)) => json_response(StatusCode::CREATED, body),
        Ok(db::LikeOutcome::AlreadyLiked(body)) => json_response(StatusCode::OK, body),
        Ok(db::LikeOutcome::PostNotFound) => json_err(StatusCode::NOT_FOUND, "post not found"),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn create_post(
    user: AuthUser,
    State(state): State<Arc<AppState>>,
    raw_body: Bytes,
) -> Response {
    let Ok(value) = serde_json::from_slice::<Value>(&raw_body) else {
        return json_err(StatusCode::BAD_REQUEST, "malformed JSON body");
    };

    let Some(raw_text) = value.get("body").and_then(Value::as_str) else {
        return json_err(StatusCode::BAD_REQUEST, "body is required");
    };

    let trimmed = raw_text.trim();
    if trimmed.is_empty() {
        return json_err(StatusCode::BAD_REQUEST, "body is required");
    }
    if trimmed.chars().count() > 500 {
        return json_err(
            StatusCode::BAD_REQUEST,
            "body must be at most 500 characters",
        );
    }

    match state
        .db
        .create(user.user_id, trimmed.to_owned(), user.username)
        .await
    {
        Ok(body) => json_response(StatusCode::CREATED, body),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn not_found() -> Response {
    json_err(StatusCode::NOT_FOUND, "not found")
}
