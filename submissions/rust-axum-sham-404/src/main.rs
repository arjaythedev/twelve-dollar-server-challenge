use axum::{
    Router,
    routing::{get, post},
};
use hmac::Mac;
use std::{env, sync::Arc, time::Instant};
use tokio::net::TcpListener;

pub mod auth;
pub mod db;
pub mod handlers;
pub mod json;

pub struct AppState {
    pub db: db::Handle,
    pub mac: auth::HmacSha256,
    pub start_time: Instant,
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    // Direct HTTP serving (no Nginx): the benchmark supplies HOST/PORT.
    let host = env::var("HOST").unwrap_or("0.0.0.0".to_string());
    let port = env::var("PORT").unwrap_or("80".to_string());
    let addr = format!("{host}:{port}");

    let jwt_secret = env::var("JWT_SECRET")
        .unwrap_or("twelve-dollar-challenge".to_string())
        .into_bytes();
    let mac = auth::HmacSha256::new_from_slice(&jwt_secret)
        .expect("JWT_SECRET cannot be used as an HMAC key");

    let db_path = env::var("SQLITE_PATH").unwrap_or_else(|_| "seed/feed.db".to_string());
    let db = db::start(&db_path).expect("failed to open sqlite db");

    let state = Arc::new(AppState {
        db,
        mac,
        start_time: Instant::now(),
    });

    let listener = TcpListener::bind(&addr)
        .await
        .expect("Failed to bind TCP Listener");

    let app = Router::new()
        .route("/health", get(handlers::health))
        .route("/posts", post(handlers::create_post))
        .route("/posts/:id", get(handlers::get_post))
        .route("/posts/:id/like", post(handlers::like_post))
        .route("/feed", get(handlers::feed))
        .fallback(handlers::not_found)
        .with_state(state);

    axum::serve(listener, app)
        .tcp_nodelay(true)
        .await
        .unwrap();
}
