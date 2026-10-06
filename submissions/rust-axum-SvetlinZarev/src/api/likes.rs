use std::sync::Arc;

use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::Response;
use tokio::sync::oneshot;

use crate::auth::authenticate;
use crate::db::{LikeOutcome, Write};
use crate::model::LikeResponse;
use crate::response::{error, json};
use crate::state::AppState;
use crate::util::parse_id;

pub async fn like_post(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Response {
    let (user_id, _) = match authenticate(&state, &headers) {
        Ok(auth) => auth,
        Err(resp) => return resp,
    };
    let post_id = match parse_id(&id) {
        Some(id) => id,
        None => return error(StatusCode::BAD_REQUEST, "invalid post id"),
    };

    let (reply, rx) = oneshot::channel();
    if state
        .writer
        .send(Write::Like {
            user_id,
            post_id,
            reply,
        })
        .await
        .is_err()
    {
        return error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error");
    }

    match rx.await {
        Ok(Ok(LikeOutcome::Created)) => json(
            StatusCode::CREATED,
            LikeResponse {
                liked: true,
                already_liked: false,
                post_id,
            },
        ),
        Ok(Ok(LikeOutcome::Existing)) => json(
            StatusCode::OK,
            LikeResponse {
                liked: true,
                already_liked: true,
                post_id,
            },
        ),
        Ok(Ok(LikeOutcome::Missing)) => error(StatusCode::NOT_FOUND, "post not found"),
        _ => error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}
