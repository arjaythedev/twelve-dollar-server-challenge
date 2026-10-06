use std::sync::Arc;

use axum::extract::State;
use axum::http::StatusCode;
use axum::response::Response;

use crate::db::{SQL_FEED, with_read};
use crate::model::{FeedResponse, Post};
use crate::response::{error, json};
use crate::state::AppState;

pub async fn feed(State(_state): State<Arc<AppState>>) -> Response {
    let posts = with_read(|conn| {
        let mut stmt = conn.prepare_cached(SQL_FEED)?;
        let rows = stmt.query_map([], Post::from_row)?;
        rows.collect::<rusqlite::Result<Vec<Post>>>()
    });
    match posts {
        Ok(posts) => json(StatusCode::OK, FeedResponse { posts }),
        Err(_) => error(StatusCode::INTERNAL_SERVER_ERROR, "internal server error"),
    }
}
