use std::sync::Arc;

use axum::{
    Json, body::Bytes, extract::{Path, State}, http::StatusCode, response::{IntoResponse, Response},
};
use serde::Serialize;
use serde_json::Value;

use crate::{AppState, auth::AuthUser, db};

#[derive(Serialize)]
struct ErrorBody<'a> {
    error: &'a str,
}

#[derive(Serialize)]
struct FeedResponse {
    posts: Vec<db::PostItem>,
}

#[derive(Serialize)]
struct PostResponse {
    post: db::PostItem,
}

#[derive(Serialize)]
struct LikeResponse {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

#[derive(Serialize)]
struct HealthOk {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct HealthDegraded {
    status: &'static str,
    db: &'static str,
    error: String,
}

pub fn json_err(status: StatusCode, msg: &'static str) -> Response {
    (status, Json(ErrorBody { error: msg })).into_response()
}

pub async fn feed() -> Response {
    match tokio::task::spawn_blocking(db::get_20_feed).await.unwrap() {
        Ok(posts) => (StatusCode::OK, Json(FeedResponse { posts })).into_response(),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn health(State(state): State<Arc<AppState>>) -> Response {
    let uptime = state.start_time.elapsed().as_secs();

    match tokio::task::spawn_blocking(db::ping).await.unwrap() {
        Ok(()) => (
            StatusCode::OK,
            Json(HealthOk {
                status: "ok",
                db: "ok",
                uptime_s: uptime,
            }),
        )
            .into_response(),
        Err(e) => (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(HealthDegraded {
                status: "degraded",
                db: "unreachable",
                error: e.to_string(),
            }),
        )
            .into_response(),
    }
}

pub fn parse_positive_id(s: &str) -> Option<i64> {
    let b = s.as_bytes();
    if b.is_empty() || !(b'1'..=b'9').contains(&b[0]) || !b[1..].iter().all(u8::is_ascii_digit) {
        return None;
    }
    s.parse::<i64>().ok()
}

pub async fn get_post(Path(post_id): Path<String>) -> Response {
    let Some(id) = parse_positive_id(&post_id) else {
        return json_err(StatusCode::BAD_REQUEST, "invalid post id");
    };

    match tokio::task::spawn_blocking(move || db::get_post(id))
        .await
        .unwrap()
    {
        Ok(Some(post)) => (StatusCode::OK, Json(PostResponse { post })).into_response(),
        Ok(None) => json_err(StatusCode::NOT_FOUND, "post not found"),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn like_post(user: AuthUser, Path(post_id): Path<String>) -> Response {
    let Some(id) = parse_positive_id(&post_id) else {
        return json_err(StatusCode::BAD_REQUEST, "invalid post id");
    };

    match tokio::task::spawn_blocking(move || db::like_post(user.user_id, id))
        .await
        .unwrap()
    {
        Ok(db::LikeResult::Created) => (
            StatusCode::CREATED,
            Json(LikeResponse {
                liked: true,
                already_liked: false,
                post_id: id,
            }),
        )
            .into_response(),
        Ok(db::LikeResult::AlreadyLiked) => (
            StatusCode::OK,
            Json(LikeResponse {
                liked: true,
                already_liked: true,
                post_id: id,
            }),
        )
            .into_response(),
        Ok(db::LikeResult::PostNotFound) => json_err(StatusCode::NOT_FOUND, "post not found"),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn create_post(user: AuthUser, raw_body: Bytes) -> Response {
    let Ok(val) = serde_json::from_slice::<Value>(&raw_body) else {
        return json_err(StatusCode::BAD_REQUEST, "malformed JSON body");
    };
    if !val.is_object() {
        return json_err(StatusCode::BAD_REQUEST, "malformed JSON body");
    }

    let Some(raw_text) = val.get("body").and_then(Value::as_str) else {
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

    let body = trimmed.to_owned();
    let body_for_db = body.clone();

    match tokio::task::spawn_blocking(move || db::create_post(user.user_id, &body_for_db))
        .await
        .unwrap()
    {
        Ok((id, created_at)) => (
            StatusCode::CREATED,
            Json(PostResponse {
                post: db::PostItem {
                    id,
                    body,
                    created_at,
                    author: user.username,
                    like_count: 0,
                },
            }),
        )
            .into_response(),
        Err(_) => json_err(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn not_found() -> Response {
    json_err(StatusCode::NOT_FOUND, "not found")
}
