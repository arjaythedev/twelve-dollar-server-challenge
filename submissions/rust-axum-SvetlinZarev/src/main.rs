mod api;
mod auth;
mod db;
mod model;
mod response;
mod state;
mod util;

use std::sync::Arc;
use std::time::Instant;

use jsonwebtoken::{Algorithm, DecodingKey, Validation};
use tokio::sync::mpsc;

use crate::db::{Write, init_read_path, writer_loop};
use crate::state::AppState;

#[global_allocator]
static GLOBAL: tikv_jemallocator::Jemalloc = tikv_jemallocator::Jemalloc;

#[tokio::main]
async fn main() {
    let path = std::env::var("SQLITE_PATH").expect("SQLITE_PATH");
    let secret = std::env::var("JWT_SECRET").expect("JWT_SECRET");
    let host = std::env::var("HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let port = std::env::var("PORT").unwrap_or_else(|_| "3000".to_string());

    let (writer, rx) = mpsc::channel::<Write>(1024);
    let writer_path = path.clone();
    std::thread::spawn(move || writer_loop(writer_path, rx));

    init_read_path(&path);

    let mut validation = Validation::new(Algorithm::HS256);
    validation.leeway = 0;
    validation.required_spec_claims.clear();
    validation.validate_exp = true;

    let state = Arc::new(AppState {
        writer,
        jwt: DecodingKey::from_secret(secret.as_bytes()),
        validation,
        start: Instant::now(),
    });

    let app = api::routes().with_state(state);

    let listener = tokio::net::TcpListener::bind(format!("{host}:{port}"))
        .await
        .expect("bind");
    axum::serve(listener, app).await.expect("serve");
}
