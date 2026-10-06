use std::sync::Arc;

use axum::body::Bytes;
use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Response;
use rusqlite::OptionalExtension;
use tokio::sync::oneshot;

use crate::auth::authenticate;
use crate::db::{SQL_POST_BY_ID, Write, with_read};
use crate::model::{Post, PostResponse};
use crate::response::{error, json};
use crate::state::AppState;
use crate::util::parse_id;

pub async fn get_post(State(_state): State<Arc<AppState>>, Path(id): Path<String>) -> Response {
    let id = match parse_id(&id) {
        Some(id) => id,
        None => return error(StatusCode::BAD_REQUEST, "invalid post id"),
    };
    let post = with_read(|conn| {
        conn.prepare_cached(SQL_POST_BY_ID)?
            .query_row([id], Post::from_row)
            .optional()
    });
    match post {
        Ok(Some(p)) => json(StatusCode::OK, PostResponse { post: p }),
        Ok(None) => error(StatusCode::NOT_FOUND, "post not found"),
        Err(_) => error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}

pub async fn create_post(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    raw: Bytes,
) -> Response {
    let (user_id, username) = match authenticate(&state, &headers) {
        Ok(auth) => auth,
        Err(resp) => return resp,
    };

    let value: serde_json::Value = match serde_json::from_slice(&raw) {
        Ok(v) => v,
        Err(_) => return error(StatusCode::BAD_REQUEST, "malformed JSON body"),
    };

    let body = match value.get("body").and_then(|v| v.as_str()) {
        Some(b) => b,
        None => return error(StatusCode::BAD_REQUEST, "body is required"),
    };
    let body = body.trim();
    if body.is_empty() {
        return error(StatusCode::BAD_REQUEST, "body is required");
    }
    if body.chars().count() > 500 {
        return error(StatusCode::BAD_REQUEST, "body must be at most 500 characters");
    }

    let (reply, rx) = oneshot::channel();
    if state
        .writer
        .send(Write::CreatePost {
            user_id,
            body: body.to_string(),
            reply,
        })
        .await
        .is_err()
    {
        return error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error");
    }

    match rx.await {
        Ok(Ok((id, created_at))) => json(
            StatusCode::CREATED,
            PostResponse {
                post: Post {
                    id,
                    body: body.to_string(),
                    created_at,
                    author: username,
                    like_count: 0,
                },
            },
        ),
        _ => error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}
