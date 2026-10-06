mod feed;
mod health;
mod likes;
mod posts;

use std::sync::Arc;

use axum::Router;
use axum::http::StatusCode;
use axum::response::Response;
use axum::routing::{get, post};

use crate::response::error;
use crate::state::AppState;

pub fn routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/health", get(health::health))
        .route("/feed", get(feed::feed))
        .route("/posts", post(posts::create_post))
        .route("/posts/{id}", get(posts::get_post))
        .route("/posts/{id}/like", post(likes::like_post))
        .fallback(not_found)
}

async fn not_found() -> Response {
    error(StatusCode::NOT_FOUND, "not found")
}
